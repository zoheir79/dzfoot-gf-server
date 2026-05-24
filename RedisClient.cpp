#include "RedisClient.h"
#include <hiredis/hiredis.h>
#include <iostream>
#include <cstring>
#include <poll.h>

RedisClient::RedisClient() = default;

RedisClient::~RedisClient() {
    unsubscribe("");
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

bool RedisClient::publishBinary(const std::string& channel, const uint8_t* data, size_t len) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ensureConnected()) return false;
    redisReply* r = (redisReply*)redisCommand(ctx_, "PUBLISH %s %b", channel.c_str(), data, len);
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

bool RedisClient::ensureSubscribed(const std::string& channel) {
    if (subCtx_ && !subCtx_->err && subscribedChannel_ == channel) return true;
    if (subCtx_) {
        redisFree(subCtx_);
        subCtx_ = nullptr;
    }
    subscribedChannel_.clear();
    if (host_.empty() || channel.empty()) return false;

    struct timeval tv{1, 0};
    subCtx_ = redisConnectWithTimeout(host_.c_str(), port_, tv);
    if (!subCtx_ || subCtx_->err) {
        if (subCtx_) {
            redisFree(subCtx_);
            subCtx_ = nullptr;
        }
        return false;
    }
    if (!password_.empty()) {
        redisReply* r = (redisReply*)redisCommand(subCtx_, "AUTH %s", password_.c_str());
        if (!r || r->type == REDIS_REPLY_ERROR) {
            if (r) freeReplyObject(r);
            redisFree(subCtx_);
            subCtx_ = nullptr;
            return false;
        }
        freeReplyObject(r);
    }
    redisReply* r = (redisReply*)redisCommand(subCtx_, "SUBSCRIBE %s", channel.c_str());
    if (!r || r->type == REDIS_REPLY_ERROR) {
        if (r) freeReplyObject(r);
        redisFree(subCtx_);
        subCtx_ = nullptr;
        return false;
    }
    freeReplyObject(r);
    subscribedChannel_ = channel;
    return true;
}

std::vector<uint8_t> RedisClient::subscribeNext(const std::string& channel, int timeoutMs) {
    int fd = -1;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!ensureSubscribed(channel)) return {};
        fd = subCtx_->fd;
    }
    // Release mutex during poll() so main thread can publish

    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;
    int ret = poll(&pfd, 1, timeoutMs);
    if (ret <= 0) return {}; // timeout or error

    std::lock_guard<std::mutex> lock(mutex_);
    // Re-check subscription still valid
    if (!subCtx_ || subCtx_->err || subscribedChannel_ != channel) return {};

    redisReply* r = nullptr;
    if (redisGetReply(subCtx_, (void**)&r) != REDIS_OK || !r) {
        redisFree(subCtx_);
        subCtx_ = nullptr;
        subscribedChannel_.clear();
        return {};
    }

    std::vector<uint8_t> result;
    if (r->type == REDIS_REPLY_ARRAY && r->elements >= 3) {
        redisReply* dataReply = r->element[2];
        if (dataReply->type == REDIS_REPLY_STRING) {
            result.assign(dataReply->str, dataReply->str + dataReply->len);
        }
    }
    freeReplyObject(r);
    return result;
}

void RedisClient::unsubscribe(const std::string& channel) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (subCtx_ && !subCtx_->err && !subscribedChannel_.empty()) {
        redisReply* r = (redisReply*)redisCommand(subCtx_, "UNSUBSCRIBE %s", subscribedChannel_.c_str());
        if (r) freeReplyObject(r);
    }
    if (subCtx_) {
        redisFree(subCtx_);
        subCtx_ = nullptr;
    }
    subscribedChannel_.clear();
}
