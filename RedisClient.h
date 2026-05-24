#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <mutex>

struct redisContext;

// Thin wrapper around hiredis providing PUBLISH, HSET, and SUBSCRIBE used by gf_server.
// Connection is established lazily on first call. Reconnects automatically on error.
// URL format: redis://[:password@]host[:port][/db]   (db ignored, defaults to 0)
class RedisClient {
public:
    RedisClient();
    ~RedisClient();

    // url: "redis://host:6379" (or with auth "redis://:pwd@host:port")
    bool configure(const std::string& url);

    // Returns true on success.
    bool publish(const std::string& channel, const std::string& message);
    bool publishBinary(const std::string& channel, const uint8_t* data, size_t len);
    bool hset(const std::string& key, const std::string& field, const std::string& value);

    // Subscribe to a channel. Returns received message (blocking with timeout).
    // Returns empty vector if timeout or error.
    std::vector<uint8_t> subscribeNext(const std::string& channel, int timeoutMs = 100);
    void unsubscribe(const std::string& channel);

    bool isConfigured() const { return !host_.empty(); }

private:
    bool ensureConnected();
    bool ensureSubscribed(const std::string& channel);
    void disconnect();

    std::mutex mutex_;
    redisContext* ctx_ = nullptr;
    redisContext* subCtx_ = nullptr; // dedicated connection for subscribe
    std::string host_;
    int port_ = 6379;
    std::string password_;
    std::string subscribedChannel_;
};
