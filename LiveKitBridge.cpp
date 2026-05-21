#include "LiveKitBridge.h"
#include <livekit/livekit.h>
#include <livekit/room.h>
#include <livekit/room_delegate.h>
#include <livekit/local_participant.h>
#include <livekit/room_event_types.h>
#include <iostream>
#include <chrono>
#include <thread>

// ------------------------------------------------------------------
// Internal RoomDelegate implementation
// ------------------------------------------------------------------
class BridgeRoomDelegate : public livekit::RoomDelegate {
public:
    explicit BridgeRoomDelegate(LiveKitBridge* bridge) : bridge_(bridge) {}

    void onConnectionStateChanged(livekit::Room* room,
                                  const livekit::ConnectionStateChangedEvent& event) override {
        (void)room;
        bridge_->onConnectionStateChanged(event);
    }

    void onUserPacketReceived(livekit::Room* room,
                              const livekit::UserDataPacketEvent& event) override {
        (void)room;
        bridge_->onUserPacketReceived(event);
    }

    void onParticipantConnected(livekit::Room* room,
                                  const livekit::ParticipantConnectedEvent& event) override {
        (void)room;
        std::cout << "[LiveKitBridge] Participant connected: " << event.participant->identity() << std::endl;
    }

    void onParticipantDisconnected(livekit::Room* room,
                                     const livekit::ParticipantDisconnectedEvent& event) override {
        (void)room;
        std::cout << "[LiveKitBridge] Participant disconnected: " << event.participant->identity() << std::endl;
    }

    void onDisconnected(livekit::Room* room,
                        const livekit::DisconnectedEvent& event) override {
        (void)room;
        (void)event;
        std::cout << "[LiveKitBridge] Room disconnected" << std::endl;
        bridge_->markDisconnected();
    }

private:
    LiveKitBridge* bridge_;
};

// ------------------------------------------------------------------
// LiveKitBridge
// ------------------------------------------------------------------
LiveKitBridge::LiveKitBridge() {
    static bool initialized = false;
    if (!initialized) {
        livekit::initialize();
        initialized = true;
    }
}

LiveKitBridge::~LiveKitBridge() {
    disconnect();
}

bool LiveKitBridge::connect(const std::string& url, const std::string& token, const std::string& roomId) {
    url_ = url;
    token_ = token;
    roomId_ = roomId;
    identity_ = "gf_server_" + roomId;

    try {
        room_ = std::make_unique<livekit::Room>();
        delegate_ = std::make_unique<BridgeRoomDelegate>(this);

        livekit::RoomOptions options;
        options.auto_subscribe = true;

        std::cout << "[LiveKitBridge] Connecting to room " << roomId_ << " as " << identity_ << std::endl;

        room_->connect(url_, token_, options);
        room_->setDelegate(delegate_.get());

        // Wait for Connected state (with 15s timeout)
        {
            std::unique_lock<std::mutex> lock(connMutex_);
            bool connected = connCv_.wait_for(lock, std::chrono::seconds(15), [this]() {
                return connectionStateChanged_ && connected_.load();
            });
            if (!connected) {
                std::cerr << "[LiveKitBridge] Connection timeout" << std::endl;
                room_->disconnect();
                room_.reset();
                delegate_.reset();
                return false;
            }
        }

        std::cout << "[LiveKitBridge] Connected to room " << roomId_ << std::endl;
        readyForData_.store(true);
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[LiveKitBridge] Connect failed: " << e.what() << std::endl;
        room_.reset();
        delegate_.reset();
        return false;
    }
}

void LiveKitBridge::disconnect() {
    readyForData_.store(false);
    connected_.store(false);
    if (room_) {
        room_->disconnect();
        room_.reset();
    }
    delegate_.reset();
}

void LiveKitBridge::publishData(const uint8_t* data, size_t len, const char* topic, bool reliable) {
    if (!connected_.load() || !room_) return;

    auto* localParticipant = room_->localParticipant();
    if (!localParticipant) return;

    std::vector<uint8_t> payload(data, data + len);
    std::string t(topic);

    try {
        // LiveKit C++ SDK publishData signature:
        // publishData(payload, reliable, destinationSids, topic)
        localParticipant->publishData(payload, reliable, {}, t);
    } catch (const std::exception& e) {
        std::cerr << "[LiveKitBridge] publishData failed: " << e.what() << std::endl;
    }
}

bool LiveKitBridge::isReadyForData() const {
    return readyForData_.load();
}

void LiveKitBridge::setOnDataReceived(DataCallback cb) {
    onDataCb_ = cb;
}

std::vector<LiveKitBridge::Packet> LiveKitBridge::drainReceived() {
    std::lock_guard<std::mutex> lock(rxMutex_);
    std::vector<Packet> out;
    out.swap(rxBuffer_);
    return out;
}

// ------------------------------------------------------------------
// Internal callbacks from BridgeRoomDelegate
// ------------------------------------------------------------------
void LiveKitBridge::onConnectionStateChanged(const livekit::ConnectionStateChangedEvent& event) {
    std::cout << "[LiveKitBridge] Connection state: " << static_cast<int>(event.state) << std::endl;

    bool wasConnected = connected_.load();
    bool nowConnected = (event.state == livekit::ConnectionState::Connected);

    if (nowConnected && !wasConnected) {
        connected_.store(true);
        readyForData_.store(true);
    } else if (!nowConnected && wasConnected) {
        connected_.store(false);
        readyForData_.store(false);
    }

    {
        std::lock_guard<std::mutex> lock(connMutex_);
        connectionStateChanged_ = true;
    }
    connCv_.notify_all();
}

void LiveKitBridge::onUserPacketReceived(const livekit::UserDataPacketEvent& event) {
    std::lock_guard<std::mutex> lock(rxMutex_);
    Packet p;
    p.topic = event.topic;
    p.data = event.data;
    p.reliable = true; // user packets are delivered reliably by default

    if (onDataCb_) {
        onDataCb_(p.topic, p.data.data(), p.data.size());
    } else {
        rxBuffer_.push_back(std::move(p));
    }
}

void LiveKitBridge::markDisconnected() {
    connected_.store(false);
    readyForData_.store(false);
}
