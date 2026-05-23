#include "LiveKitBridge.h"
#include <rtc/rtc.hpp>
#include <nlohmann/json.hpp>
#include <iostream>

LiveKitBridge::LiveKitBridge() {
    rtc::InitLogger(rtc::LogLevel::Warning);
}

LiveKitBridge::~LiveKitBridge() {
    disconnect();
}

bool LiveKitBridge::connect(const std::string& url, const std::string& token, const std::string& roomId) {
    std::string wsBase = url;
    if (wsBase.compare(0, 8, "https://") == 0) {
        wsBase.replace(0, 5, "wss");
    } else if (wsBase.compare(0, 7, "http://") == 0) {
        wsBase.replace(0, 4, "ws");
    }
    url_ = wsBase;
    token_ = token;
    roomId_ = roomId;
    identity_ = "gf_server_" + roomId;

    try {
        rtc::WebSocketConfiguration wsConfig;
        wsConfig.protocols = {"livekit-protocol"};
        ws_ = std::make_unique<rtc::WebSocket>(wsConfig);

        ws_->onOpen([this]() {
            std::cout << "[LiveKitBridge] WS open, joining room " << roomId_ << std::endl;
            nlohmann::json joinMsg = {
                {"join", {
                    {"room", roomId_},
                    {"identity", identity_}
                }}
            };
            ws_->send(joinMsg.dump());
        });

        ws_->onMessage([this](rtc::message_variant msg) {
            if (std::holds_alternative<std::string>(msg)) {
                std::string raw = std::get<std::string>(msg);
                std::cout << "[LiveKitBridge] WS msg: " << raw.substr(0, 200) << std::endl;
                handleSignaling(raw);
            } else if (std::holds_alternative<std::vector<uint8_t>>(msg)) {
                auto& data = std::get<std::vector<uint8_t>>(msg);
                std::cout << "[LiveKitBridge] WS binary msg: " << data.size() << " bytes" << std::endl;
                std::string raw(data.begin(), data.end());
                handleSignaling(raw);
            }
        });

        ws_->onError([](const std::string& err) {
            std::cerr << "[LiveKitBridge] WS error: " << err << std::endl;
        });

        ws_->onClosed([]() {
            std::cout << "[LiveKitBridge] WS closed" << std::endl;
        });

        // LiveKit signaling endpoint: token passed as query param
        std::string wsUrl = url_ + "/rtc?access_token=" + token_;
        ws_->open(wsUrl);

        connected_.store(true);
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[LiveKitBridge] Connect failed: " << e.what() << std::endl;
        return false;
    }
}

void LiveKitBridge::handleSignaling(const std::string& msg) {
    try {
        auto j = nlohmann::json::parse(msg);

        if (j.contains("offer")) {
            std::string sdp = j["offer"]["sdp"];
            rtc::Configuration cfg;
            cfg.iceServers.emplace_back("stun:stun.l.google.com:19302");

            pc_ = std::make_shared<rtc::PeerConnection>(cfg);

            pc_->onStateChange([](rtc::PeerConnection::State s) {
                std::cout << "[LiveKitBridge] PC state: " << static_cast<int>(s) << std::endl;
            });

            pc_->onGatheringStateChange([this](rtc::PeerConnection::GatheringState s) {
                if (s == rtc::PeerConnection::GatheringState::Complete) {
                    sendAnswer();
                }
            });

            pc_->onDataChannel([this](std::shared_ptr<rtc::DataChannel> dc) {
                setupDataChannel(dc, dc->label());
            });

            // Set remote description (offer from LiveKit server)
            pc_->setRemoteDescription(rtc::Description(sdp, "offer"));

            // Create data channels: gs = unreliable (game state, freshness > reliability)
            rtc::DataChannelInit gsInit;
            gsInit.reliability.type = rtc::Reliability::Type::Rexmit;
            gsInit.reliability.rexmit = 0;  // 0 retransmits = unreliable
            dcGs_ = pc_->createDataChannel("gs", gsInit);
            setupDataChannel(dcGs_, "gs");

            // ev = reliable (events must arrive)
            rtc::DataChannelInit evInit;
            evInit.reliability.type = rtc::Reliability::Type::Reliable;
            dcEv_ = pc_->createDataChannel("ev", evInit);
            setupDataChannel(dcEv_, "ev");

            // in = reliable (inputs must not be dropped)
            rtc::DataChannelInit inInit;
            inInit.reliability.type = rtc::Reliability::Type::Reliable;
            dcIn_ = pc_->createDataChannel("in", inInit);
            setupDataChannel(dcIn_, "in");

        } else if (j.contains("trickle") && pc_) {
            std::string candidate = j["trickle"]["candidate"];
            std::string sdpMid = j.value("/trickle/sdpMid"_json_pointer, "");
            if (!candidate.empty()) {
                pc_->addRemoteCandidate(rtc::Candidate(candidate, sdpMid));
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "[LiveKitBridge] Signal parse error: " << e.what() << std::endl;
    }
}

void LiveKitBridge::sendAnswer() {
    if (!pc_ || !ws_) return;
    auto desc = pc_->localDescription();
    if (!desc) return;

    nlohmann::json answer = {
        {"answer", {
            {"type", "answer"},
            {"sdp", std::string(desc.value())}
        }}
    };
    ws_->send(answer.dump());
}

void LiveKitBridge::setupDataChannel(std::shared_ptr<rtc::DataChannel> dc, const std::string& label) {
    if (!dc) return;

    dc->onOpen([label]() {
        std::cout << "[LiveKitBridge] DC '" << label << "' open" << std::endl;
    });

    dc->onMessage([this, label](rtc::message_variant msg) {
        if (std::holds_alternative<rtc::binary>(msg)) {
            auto& bin = std::get<rtc::binary>(msg);
            std::lock_guard<std::mutex> lock(cbMutex_);
            if (onDataCb_) {
                onDataCb_(label, reinterpret_cast<const uint8_t*>(bin.data()), bin.size());
            } else {
                std::lock_guard<std::mutex> lock(rxMutex_);
                Packet p;
                p.topic = label;
                p.data.assign(reinterpret_cast<const uint8_t*>(bin.data()),
                              reinterpret_cast<const uint8_t*>(bin.data()) + bin.size());
                rxBuffer_.push_back(std::move(p));
            }
        }
    });
}

void LiveKitBridge::publishData(const uint8_t* data, size_t len, const char* topic, bool /*reliable*/) {
    if (!connected_.load()) return;

    // Route by topic semantics:
    //   "gs"                  -> unreliable (freshness wins over reliability)
    //   "ev" | "setup" | "in" -> reliable (events, match setup, inputs must arrive)
    std::string t(topic);
    std::shared_ptr<rtc::DataChannel> dc;
    if (t == "gs")                                  dc = dcGs_;
    else if (t == "ev" || t == "setup" || t == "in") dc = dcEv_;
    else                                             dc = dcEv_; // default reliable

    if (dc && dc->isOpen()) {
        rtc::binary msg(reinterpret_cast<const std::byte*>(data),
                        reinterpret_cast<const std::byte*>(data) + len);
        dc->send(msg);
    }
}

bool LiveKitBridge::isReadyForData() const {
    // "ready" = reliable channel open (so MatchSetup can be sent)
    return dcEv_ && dcEv_->isOpen();
}

void LiveKitBridge::disconnect() {
    connected_.store(false);
    readyForData_.store(false);
    dcGs_.reset();
    dcEv_.reset();
    dcIn_.reset();
    pc_.reset();
    if (ws_) {
        ws_->close();
        ws_.reset();
    }
}

void LiveKitBridge::setOnDataReceived(DataCallback cb) {
    std::lock_guard<std::mutex> lock(cbMutex_);
    onDataCb_ = std::move(cb);
}

std::vector<LiveKitBridge::Packet> LiveKitBridge::drainReceived() {
    std::lock_guard<std::mutex> lock(rxMutex_);
    std::vector<Packet> out;
    out.swap(rxBuffer_);
    return out;
}
