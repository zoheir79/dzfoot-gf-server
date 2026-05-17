#include "LiveKitBridge.h"
#include <iostream>

bool LiveKitBridge::connect(const std::string& url, const std::string& token) {
    // TODO: integrate livekit::client-sdk-cpp
    // Room::connect(url, token)
    // room->setDataReceivedCallback(...)
    std::cout << "[LiveKitBridge] Connecting to " << url << " as server bot..." << std::endl;
    connected_ = true;
    return true;
}

void LiveKitBridge::disconnect() {
    connected_ = false;
    std::cout << "[LiveKitBridge] Disconnected" << std::endl;
}

void LiveKitBridge::publishData(const uint8_t* data, size_t len, const char* topic, bool reliable) {
    if (!connected_) return;
    // TODO: room->localParticipant()->publishData(data, len, topic, reliable)
}

void LiveKitBridge::setOnDataReceived(DataCallback cb) {
    onDataCb_ = cb;
}
