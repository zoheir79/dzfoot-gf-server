#pragma once
#include <string>
#include <mutex>

struct redisContext;

// Thin wrapper around hiredis providing PUBLISH and HSET used by gf_server.
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
    bool hset(const std::string& key, const std::string& field, const std::string& value);

    bool isConfigured() const { return !host_.empty(); }

private:
    bool ensureConnected();
    void disconnect();

    std::mutex mutex_;
    redisContext* ctx_ = nullptr;
    std::string host_;
    int port_ = 6379;
    std::string password_;
};
