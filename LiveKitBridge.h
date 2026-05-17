#pragma once
#include <string>
#include <functional>
#include <cstdint>

class LiveKitBridge {
public:
    using DataCallback = std::function<void(const uint8_t* data, size_t len)>;

    bool connect(const std::string& url, const std::string& token);
    void disconnect();

    void publishData(const uint8_t* data, size_t len, const char* topic, bool reliable);

    void setOnDataReceived(DataCallback cb);

    bool isConnected() const { return connected_; }

private:
    bool connected_ = false;
    DataCallback onDataCb_;
    std::string roomId_;
    std::string identity_;
};
