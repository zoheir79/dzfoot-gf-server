#include "LiveKitBridge.h"
#include <rtc/rtc.hpp>
#include "livekit_rtc.pb.h"
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
        ws_ = std::make_unique<rtc::WebSocket>();

        ws_->onOpen([this]() {
            std::cout << "[LiveKitBridge] WS open, connection established for room " << roomId_ << std::endl;
        });

        ws_->onMessage([this](rtc::message_variant msg) {
            if (std::holds_alternative<rtc::binary>(msg)) {
                auto bin = std::get<rtc::binary>(msg);
                handleSignaling(reinterpret_cast<const uint8_t*>(bin.data()), bin.size());
            } else if (std::holds_alternative<std::string>(msg)) {
                std::string raw = std::get<std::string>(msg);
                if (!raw.empty() && raw[0] == '{') {
                    std::cout << "[LiveKitBridge] WS text (ignored): " << raw.substr(0, 200) << std::endl;
                }
            }
        });

        ws_->onError([](const std::string& err) {
            std::cerr << "[LiveKitBridge] WS error: " << err << std::endl;
        });

        ws_->onClosed([]() {
            std::cout << "[LiveKitBridge] WS closed" << std::endl;
        });

        std::string wsUrl = url_ + "/rtc?access_token=" + token_;
        ws_->open(wsUrl);

        connected_.store(true);
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[LiveKitBridge] Connect failed: " << e.what() << std::endl;
        return false;
    }
}

void LiveKitBridge::handleSignaling(const uint8_t* data, size_t len) {
    livekit::SignalResponse resp;
    if (!resp.ParseFromArray(data, static_cast<int>(len))) {
        std::cerr << "[LiveKitBridge] Failed to parse SignalResponse protobuf" << std::endl;
        return;
    }

    std::cout << "[LiveKitBridge] SignalResponse case=" << resp.message_case() << std::endl;

    if (resp.has_join()) {
        std::cout << "[LiveKitBridge] Join response received, room=" << resp.join().room().name() << std::endl;
        createPublisherPC();
    } else if (resp.has_answer() && pcPublisher_) {
        std::string sdp = resp.answer().sdp();
        std::cout << "[LiveKitBridge] Received answer for publisher, SDP len=" << sdp.size() << std::endl;
        pcPublisher_->setRemoteDescription(rtc::Description(sdp, "answer"));
    } else if (resp.has_offer()) {
        std::string sdp = resp.offer().sdp();
        std::cout << "[LiveKitBridge] Received subscriber offer, SDP len=" << sdp.size() << std::endl;

        rtc::Configuration cfg;
        cfg.iceServers.emplace_back("stun:stun.l.google.com:19302");

        pcSubscriber_ = std::make_shared<rtc::PeerConnection>(cfg);

        pcSubscriber_->onStateChange([](rtc::PeerConnection::State s) {
            std::cout << "[LiveKitBridge] Subscriber PC state: " << static_cast<int>(s) << std::endl;
        });

        pcSubscriber_->onGatheringStateChange([this](rtc::PeerConnection::GatheringState s) {
            if (s == rtc::PeerConnection::GatheringState::Complete) {
                sendAnswer();
            }
        });

        pcSubscriber_->onDataChannel([this](std::shared_ptr<rtc::DataChannel> dc) {
            setupDataChannel(dc, dc->label());
        });

        pcSubscriber_->setRemoteDescription(rtc::Description(sdp, "offer"));

        // Create input data channel on subscriber (server may also create it)
        rtc::DataChannelInit inInit;
        inInit.reliability.type = rtc::Reliability::Type::Reliable;
        dcIn_ = pcSubscriber_->createDataChannel("in", inInit);
        setupDataChannel(dcIn_, "in");

    } else if (resp.has_trickle()) {
        std::string candidate = resp.trickle().candidateinit();
        int target = resp.trickle().target();
        if (!candidate.empty()) {
            std::cout << "[LiveKitBridge] Remote ICE candidate (target=" << target << "): " << candidate.substr(0, 80) << std::endl;
            if (target == livekit::SignalTarget::PUBLISHER && pcPublisher_) {
                pcPublisher_->addRemoteCandidate(rtc::Candidate(candidate, "0"));
            } else if (target == livekit::SignalTarget::SUBSCRIBER && pcSubscriber_) {
                pcSubscriber_->addRemoteCandidate(rtc::Candidate(candidate, "0"));
            }
        }
    }
}

void LiveKitBridge::createPublisherPC() {
    rtc::Configuration cfg;
    // No STUN/TURN - use host candidates only (GF server and LiveKit SFU are on same network)

    pcPublisher_ = std::make_shared<rtc::PeerConnection>(cfg);

    pcPublisher_->onStateChange([](rtc::PeerConnection::State s) {
        std::cout << "[LiveKitBridge] Publisher PC state: " << static_cast<int>(s) << std::endl;
    });

    pcPublisher_->onLocalDescription([this](rtc::Description desc) {
        std::cout << "[LiveKitBridge] Publisher local description ready, sending offer" << std::endl;
        sendOffer();
    });

    // Create data channels on publisher: gs (unreliable), ev (reliable)
    rtc::DataChannelInit gsInit;
    gsInit.reliability.type = rtc::Reliability::Type::Rexmit;
    gsInit.reliability.rexmit = 0;
    dcGs_ = pcPublisher_->createDataChannel("gs", gsInit);
    setupDataChannel(dcGs_, "gs");

    rtc::DataChannelInit evInit;
    evInit.reliability.type = rtc::Reliability::Type::Reliable;
    dcEv_ = pcPublisher_->createDataChannel("ev", evInit);
    setupDataChannel(dcEv_, "ev");

    std::cout << "[LiveKitBridge] Publisher PC created, waiting for ICE gathering..." << std::endl;
}

void LiveKitBridge::sendOffer() {
    if (!pcPublisher_ || !ws_) return;
    auto desc = pcPublisher_->localDescription();
    if (!desc) return;

    livekit::SignalRequest req;
    auto* offer = req.mutable_offer();
    offer->set_type("offer");
    offer->set_sdp(std::string(desc.value()));

    std::string bin;
    if (!req.SerializeToString(&bin)) {
        std::cerr << "[LiveKitBridge] Failed to serialize offer" << std::endl;
        return;
    }

    rtc::binary msg;
    msg.reserve(bin.size());
    for (char c : bin) msg.push_back(static_cast<std::byte>(c));
    ws_->send(msg);
    std::cout << "[LiveKitBridge] Sent publisher offer (protobuf), len=" << bin.size() << std::endl;
}

void LiveKitBridge::sendAnswer() {
    if (!pcSubscriber_ || !ws_) return;
    auto desc = pcSubscriber_->localDescription();
    if (!desc) return;

    livekit::SignalRequest req;
    auto* answer = req.mutable_answer();
    answer->set_type("answer");
    answer->set_sdp(std::string(desc.value()));

    std::string bin;
    if (!req.SerializeToString(&bin)) {
        std::cerr << "[LiveKitBridge] Failed to serialize answer" << std::endl;
        return;
    }

    rtc::binary msg;
    msg.reserve(bin.size());
    for (char c : bin) msg.push_back(static_cast<std::byte>(c));
    ws_->send(msg);
    std::cout << "[LiveKitBridge] Sent subscriber answer (protobuf), len=" << bin.size() << std::endl;
}

void LiveKitBridge::sendTrickle(const std::string& candidate) {
    if (!ws_) return;

    livekit::SignalRequest req;
    auto* trickle = req.mutable_trickle();
    trickle->set_candidateinit(candidate);
    trickle->set_target(livekit::SignalTarget::PUBLISHER);

    std::string bin;
    if (!req.SerializeToString(&bin)) {
        std::cerr << "[LiveKitBridge] Failed to serialize trickle" << std::endl;
        return;
    }

    rtc::binary msg;
    msg.reserve(bin.size());
    for (char c : bin) msg.push_back(static_cast<std::byte>(c));
    ws_->send(msg);
    std::cout << "[LiveKitBridge] Sent trickle (protobuf), len=" << bin.size() << std::endl;
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

    std::string t(topic);
    std::shared_ptr<rtc::DataChannel> dc;
    if (t == "gs")                                  dc = dcGs_;
    else if (t == "ev" || t == "setup" || t == "in") dc = dcEv_;
    else                                             dc = dcEv_;

    if (dc && dc->isOpen()) {
        rtc::binary msg(reinterpret_cast<const std::byte*>(data),
                        reinterpret_cast<const std::byte*>(data) + len);
        dc->send(msg);
    }
}

bool LiveKitBridge::isReadyForData() const {
    return dcEv_ && dcEv_->isOpen();
}

void LiveKitBridge::disconnect() {
    connected_.store(false);
    readyForData_.store(false);
    dcGs_.reset();
    dcEv_.reset();
    dcIn_.reset();
    pcPublisher_.reset();
    pcSubscriber_.reset();
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
