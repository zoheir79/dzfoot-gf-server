#pragma once
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <functional>
#include <memory>
#include <condition_variable>

// LiveKit C++ SDK forward declarations
namespace livekit {
    class Room;
    class RoomDelegate;
    struct RoomOptions;
    struct UserDataPacketEvent;
    struct ConnectionStateChangedEvent;
}

// libdatachannel forward declarations (fallback)
namespace rtc {
    class WebSocket;
    class PeerConnection;
    class DataChannel;
}

class LiveKitBridge {
public:
    struct Packet {
        std::string topic;
        std::vector<uint8_t> data;
        bool reliable = false;
    };

    using DataCallback = std::function<void(const std::string& topic, const uint8_t* data, size_t len)>;

    LiveKitBridge();
    ~LiveKitBridge();

    // Connect to LiveKit room (blocking, with timeout)
    bool connect(const std::string& url, const std::string& token, const std::string& roomId);
    void disconnect();

    // Publish binary data on topic ("gs" = unreliable GameState, "ev" = reliable events)
    void publishData(const uint8_t* data, size_t len, const char* topic, bool reliable);

    // Register callback for incoming data on any topic (primarily "in")
    void setOnDataReceived(DataCallback cb);

    // Drain buffered packets (used when callback is not set)
    std::vector<Packet> drainReceived();

    bool isConnected() const { return connected_.load(); }
    // True when the room is connected and data channels are ready.
    bool isReadyForData() const;
    const std::string& getRoomId() const { return roomId_; }

private:
    std::string url_;
    std::string token_;
    std::string roomId_;
    std::string identity_;
    std::atomic<bool> connected_{false};
    std::atomic<bool> readyForData_{false};

#define USE_LIVEKIT_SDK
#ifdef USE_LIVEKIT_SDK
    // Official SDK members
    std::unique_ptr<livekit::Room> room_;
    std::unique_ptr<livekit::RoomDelegate> delegate_;

    // Connection synchronization
    std::mutex connMutex_;
    std::condition_variable connCv_;
    bool connectionStateChanged_ = false;

    // Internal callbacks (invoked by BridgeRoomDelegate)
    void onConnectionStateChanged(const struct livekit::ConnectionStateChangedEvent& event);
    void onUserPacketReceived(const struct livekit::UserDataPacketEvent& event);
    void markDisconnected();
#else
    // libdatachannel fallback members
    std::unique_ptr<rtc::WebSocket> ws_;
    std::shared_ptr<rtc::PeerConnection> pcPublisher_;
    std::shared_ptr<rtc::PeerConnection> pcSubscriber_;
    std::shared_ptr<rtc::DataChannel> dcGs_;
    std::shared_ptr<rtc::DataChannel> dcEv_;
    std::shared_ptr<rtc::DataChannel> dcIn_;

    void handleSignaling(const uint8_t* data, size_t len);
    void createPublisherPC();
    void sendOffer();
    void sendAnswer();
    void sendTrickle(const std::string& candidate);
    void setupDataChannel(std::shared_ptr<rtc::DataChannel> dc, const std::string& label);
#endif

    mutable std::mutex cbMutex_;
    DataCallback onDataCb_;
    std::mutex rxMutex_;
    std::vector<Packet> rxBuffer_;
};
