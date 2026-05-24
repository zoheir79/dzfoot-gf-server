#include "LiveKitBridge.h"
#include <livekit/room.h>
#include <livekit/local_participant.h>
#include <livekit/room_delegate.h>
#include <livekit/room_event_types.h>
#include <iostream>
#include <thread>
#include <chrono>

// BridgeRoomDelegate: forwards LiveKit SDK callbacks to LiveKitBridge
class BridgeRoomDelegate : public livekit::RoomDelegate {
public:
    LiveKitBridge* bridge;

    void onConnectionStateChanged(livekit::Room&, const livekit::ConnectionStateChangedEvent& event) override {
        if (bridge) {
            bridge->onConnectionStateChanged(event);
        }
    }

    void onUserPacketReceived(livekit::Room&, const livekit::UserDataPacketEvent& event) override {
        if (bridge) {
            bridge->onUserPacketReceived(event);
        }
    }

    void onDisconnected(livekit::Room&, const livekit::DisconnectedEvent&) override {
        if (bridge) {
            bridge->markDisconnected();
        }
    }
};

LiveKitBridge::LiveKitBridge() {
    delegate_ = std::make_unique<BridgeRoomDelegate>();
    static_cast<BridgeRoomDelegate*>(delegate_.get())->bridge = this;
}

LiveKitBridge::~LiveKitBridge() {
    disconnect();
}

bool LiveKitBridge::connect(const std::string& url, const std::string& token, const std::string& roomId) {
    url_ = url;
    token_ = token;
    roomId_ = roomId;
    identity_ = "gf_server_" + roomId;

    // Convert https:// to wss://
    std::string wsUrl = url;
    if (wsUrl.compare(0, 8, "https://") == 0) {
        wsUrl.replace(0, 5, "wss");
    } else if (wsUrl.compare(0, 7, "http://") == 0) {
        wsUrl.replace(0, 4, "ws");
    }

    try {
        room_ = std::make_unique<livekit::Room>();
        room_->setDelegate(static_cast<BridgeRoomDelegate*>(delegate_.get()));

        livekit::RoomOptions options;
        options.auto_subscribe = true;
        options.single_peer_connection = true;

        std::cout << "[LiveKitBridge] Connecting to " << wsUrl << " room=" << roomId << std::endl;

        bool ok = room_->connect(wsUrl, token, options);
        if (!ok) {
            std::cerr << "[LiveKitBridge] Room::connect returned false" << std::endl;
            return false;
        }

        // Wait for connection state to become Connected
        {
            std::unique_lock<std::mutex> lock(connMutex_);
            bool success = connCv_.wait_for(lock, std::chrono::seconds(15), [this] {
                return connectionStateChanged_;
            });
            if (!success || !connected_.load()) {
                std::cerr << "[LiveKitBridge] Connection timeout or failed" << std::endl;
                return false;
            }
        }

        readyForData_.store(true);
        std::cout << "[LiveKitBridge] Connected to room " << roomId << std::endl;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[LiveKitBridge] Connect exception: " << e.what() << std::endl;
        return false;
    }
}

void LiveKitBridge::disconnect() {
    readyForData_.store(false);
    connected_.store(false);
    if (room_) {
        room_.reset();
    }
}

void LiveKitBridge::publishData(const uint8_t* data, size_t len, const char* topic, bool reliable) {
    if (!room_ || !readyForData_.load()) return;

    auto* local = room_->localParticipant();
    if (!local) return;

    try {
        std::vector<std::uint8_t> payload(data, data + len);
        std::string t(topic);
        local->publishData(payload, reliable, {}, t);
    } catch (const std::exception& e) {
        std::cerr << "[LiveKitBridge] publishData error: " << e.what() << std::endl;
    }
}

bool LiveKitBridge::isReadyForData() const {
    return readyForData_.load();
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

// Called by BridgeRoomDelegate
void LiveKitBridge::onConnectionStateChanged(const livekit::ConnectionStateChangedEvent& event) {
    std::cout << "[LiveKitBridge] Connection state: " << static_cast<int>(event.state) << std::endl;
    if (event.state == livekit::ConnectionState::Connected) {
        connected_.store(true);
        {
            std::lock_guard<std::mutex> lock(connMutex_);
            connectionStateChanged_ = true;
        }
        connCv_.notify_all();
    } else if (event.state == livekit::ConnectionState::Disconnected) {
        connected_.store(false);
        readyForData_.store(false);
    }
}

void LiveKitBridge::onUserPacketReceived(const livekit::UserDataPacketEvent& event) {
    std::lock_guard<std::mutex> lock(cbMutex_);
    if (onDataCb_) {
        onDataCb_(event.topic, event.data.data(), event.data.size());
    } else {
        std::lock_guard<std::mutex> lock(rxMutex_);
        Packet p;
        p.topic = event.topic;
        p.data = event.data;
        rxBuffer_.push_back(std::move(p));
    }
}

void LiveKitBridge::markDisconnected() {
    connected_.store(false);
    readyForData_.store(false);
}
