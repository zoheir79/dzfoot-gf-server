#include "RedisClient.h"
#include <hiredis/hiredis.h>
#include <iostream>
#include <cstring>

RedisClient::RedisClient() = default;

RedisClient::~RedisClient() {
    disconnect();
}

// Parse "redis://[:pwd@]host[:port]"
bool RedisClient::configure(const std::string& url) {
    std::lock_guard<std::mutex> lock(mutex_);
    disconnect();

    const std::string scheme = "redis://";
    if (url.compare(0, scheme.size(), scheme) != 0) {
        std::cerr << "[RedisClient] Invalid URL (need redis:// prefix): " << url << std::endl;
        return false;
    }
    std::string rest = url.substr(scheme.size());

    // Strip optional /db suffix
    auto slash = rest.find('/');
    if (slash != std::string::npos) rest = rest.substr(0, slash);

    // Optional auth: [:pwd@]
    auto at = rest.find('@');
    if (at != std::string::npos) {
        std::string auth = rest.substr(0, at);
        rest = rest.substr(at + 1);
        if (!auth.empty() && auth[0] == ':') auth = auth.substr(1);
        password_ = auth;
    } else {
        password_.clear();
    }

    // host[:port]
    auto colon = rest.find(':');
    if (colon != std::string::npos) {
        host_ = rest.substr(0, colon);
        try { port_ = std::stoi(rest.substr(colon + 1)); }
        catch (...) { port_ = 6379; }
    } else {
        host_ = rest;
        port_ = 6379;
    }

    return !host_.empty();
}

bool RedisClient::ensureConnected() {
    if (ctx_ && !ctx_->err) return true;
    if (ctx_) {
        redisFree(ctx_);
        ctx_ = nullptr;
    }
    if (host_.empty()) return false;

    struct timeval tv{1, 0};
    ctx_ = redisConnectWithTimeout(host_.c_str(), port_, tv);
    if (!ctx_ || ctx_->err) {
        if (ctx_) {
            std::cerr << "[RedisClient] Connect failed: " << ctx_->errstr << std::endl;
            redisFree(ctx_);
            ctx_ = nullptr;
        }
        return false;
    }
    if (!password_.empty()) {
        redisReply* r = (redisReply*)redisCommand(ctx_, "AUTH %s", password_.c_str());
        if (!r || r->type == REDIS_REPLY_ERROR) {
            std::cerr << "[RedisClient] AUTH failed" << std::endl;
            if (r) freeReplyObject(r);
            disconnect();
            return false;
        }
        freeReplyObject(r);
    }
    return true;
}

void RedisClient::disconnect() {
    if (ctx_) {
        redisFree(ctx_);
        ctx_ = nullptr;
    }
}

bool RedisClient::publish(const std::string& channel, const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ensureConnected()) return false;
    redisReply* r = (redisReply*)redisCommand(ctx_, "PUBLISH %s %s", channel.c_str(), message.c_str());
    if (!r) {
        disconnect();
        return false;
    }
    bool ok = (r->type != REDIS_REPLY_ERROR);
    freeReplyObject(r);
    return ok;
}

bool RedisClient::hset(const std::string& key, const std::string& field, const std::string& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ensureConnected()) return false;
    redisReply* r = (redisReply*)redisCommand(ctx_, "HSET %s %s %s", key.c_str(), field.c_str(), value.c_str());
    if (!r) {
        disconnect();
        return false;
    }
    bool ok = (r->type != REDIS_REPLY_ERROR);
    freeReplyObject(r);
    return ok;
}
