#include "GameServer.h"
#include "LiveKitBridge.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <cstring>
#include <cmath>
#include <cctype>

namespace GameServer {

// Validate Redis CLI arguments to prevent shell injection
// TODO: migrate to hiredis C client for production
static bool isSafeRedisArg(const std::string& s) {
    for (char c : s) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_' && c != ':' && c != '/' && c != '.' && c != '@') {
            return false;
        }
    }
    return true;
}

static void redisPublish(const std::string& roomId, const std::string& redisUrl, const char* channel) {
    if (!isSafeRedisArg(roomId) || !isSafeRedisArg(redisUrl)) {
        std::cerr << "[GameServer] Unsafe Redis args, skipping publish" << std::endl;
        return;
    }
    std::string cmd = "redis-cli -u " + redisUrl + " PUBLISH " + channel + " " + roomId + " > /dev/null 2>&1";
    system(cmd.c_str());
}

static void redisHeartbeat(const std::string& roomId, const std::string& redisUrl) {
    if (!isSafeRedisArg(roomId) || !isSafeRedisArg(redisUrl)) return;
    std::string cmd = "redis-cli -u " + redisUrl + " HSET gf.heartbeat " + roomId + " $(date +%s) > /dev/null 2>&1";
    system(cmd.c_str());
}

Server::Server(const Config& cfg) : cfg_(cfg) {
    currentState_.timer = 0.0f;
    currentState_.tick = 0;
    currentState_.gameMode = 0;  // NORMAL
    currentState_.score[0] = 0;
    currentState_.score[1] = 0;
    startTime_ = std::chrono::steady_clock::now();

    // Initialize mock player positions on pitch
    for (int i = 0; i < 11; ++i) {
        currentState_.players[i].pos[0] = -10.0f + i * 2.0f;
        currentState_.players[i].pos[1] = 0.0f;
        currentState_.players[i].pos[2] = 0.0f;
        currentState_.players[i].team = 0;
        currentState_.players[i].anim = 0;
    }
    for (int i = 11; i < 22; ++i) {
        currentState_.players[i].pos[0] = 10.0f - (i - 11) * 2.0f;
        currentState_.players[i].pos[1] = 0.0f;
        currentState_.players[i].pos[2] = 5.0f;
        currentState_.players[i].team = 1;
        currentState_.players[i].anim = 0;
    }
    currentState_.ball.pos[0] = 0.0f;
    currentState_.ball.pos[1] = 0.5f;
    currentState_.ball.pos[2] = 0.0f;
}

void Server::run() {
    std::cout << "[GameServer] Room " << cfg_.roomId << " starting (" << cfg_.duration << "s)" << std::endl;

    // Signal ready to Session Service via Redis
    if (!cfg_.redisUrl.empty()) {
        redisPublish(cfg_.roomId, cfg_.redisUrl, "gf.ready");
    }

    while (running_) {
        auto frameStart = std::chrono::steady_clock::now();

        tick();
        broadcastGameState();

        // Heartbeat every ~1 second
        if (tickCounter_ % 60 == 0 && !cfg_.redisUrl.empty()) {
            redisHeartbeat(cfg_.roomId, cfg_.redisUrl);
        }

        auto elapsed = std::chrono::steady_clock::now() - frameStart;
        auto sleepTime = std::chrono::milliseconds(16) - elapsed;
        if (sleepTime.count() > 0) {
            std::this_thread::sleep_for(sleepTime);
        }
    }
}

void Server::tick() {
    ++tickCounter_;
    currentState_.tick = tickCounter_;
    currentState_.timer = tickCounter_ * (1.0f / 60.0f);

    // Mock physics: move ball in sine wave
    currentState_.ball.pos[0] = std::sin(currentState_.timer * 0.5f) * 15.0f;
    currentState_.ball.pos[2] = std::cos(currentState_.timer * 0.3f) * 10.0f;

    // Update player animations
    for (int i = 0; i < 22; ++i) {
        currentState_.players[i].anim = deduceAnimId(currentState_.players[i], currentState_.ball);
    }

    processInputs();
    accumulateStats();
    detectEvents();

    // Half-time / full-time events
    int half = cfg_.duration / 2;
    if (std::abs(currentState_.timer - half) < 0.02f) {
        MatchEvent ev{4, 0, 0, {0,0,0}, tickCounter_, {currentState_.score[0], currentState_.score[1]}};
        eventQueue_.push_back(ev);
    }

    if (currentState_.timer >= cfg_.duration) {
        MatchEvent ev{5, 0, 0, {0,0,0}, tickCounter_, {currentState_.score[0], currentState_.score[1]}};
        eventQueue_.push_back(ev);
        running_ = false;
    }
}

uint8_t Server::deduceAnimId(const GameState::PlayerState& p, const GameState::BallState& ball) {
    float speed = p.vel[0]*p.vel[0] + p.vel[2]*p.vel[2];
    if (speed < 0.01f) return 0;   // IDLE
    if (speed < 0.16f) return 1;    // WALK
    if (speed < 0.64f) return 2;    // RUN
    return 3;                        // SPRINT
}

void Server::broadcastGameState() {
    if (!cfg_.livekitBridge) return;
    // Serialize currentState_ to binary and broadcast via topic "gs"
    cfg_.livekitBridge->publishData((const uint8_t*)&currentState_, sizeof(currentState_), "gs", false);

    // Send any pending events via topic "ev" (reliable)
    for (const auto& ev : eventQueue_) {
        cfg_.livekitBridge->publishData((const uint8_t*)&ev, sizeof(ev), "ev", true);
    }
    eventQueue_.clear();
}

void Server::processInputs() {
    // TODO: receive inputs from LiveKit topic "in"
    // Apply to player velocities/actions
}

void Server::accumulateStats() {
    // Accumulate possession per tick based on ball proximity
    float dA = 9999.0f, dB = 9999.0f;
    for (int i = 0; i < 11; ++i) {
        float dx = currentState_.players[i].pos[0] - currentState_.ball.pos[0];
        float dz = currentState_.players[i].pos[2] - currentState_.ball.pos[2];
        dA = std::min(dA, dx*dx + dz*dz);
    }
    for (int i = 11; i < 22; ++i) {
        float dx = currentState_.players[i].pos[0] - currentState_.ball.pos[0];
        float dz = currentState_.players[i].pos[2] - currentState_.ball.pos[2];
        dB = std::min(dB, dx*dx + dz*dz);
    }
    if (dA < dB) stats_.possession_ticks[0] += 1.0f;
    else stats_.possession_ticks[1] += 1.0f;
}

void Server::detectEvents() {
    // Mock: random goal every ~200 ticks for demo
    if (tickCounter_ % 200 == 0 && tickCounter_ > 0) {
        int team = (tickCounter_ / 200) % 2;
        currentState_.score[team]++;
        MatchEvent ev{0, (uint8_t)team, 0, {currentState_.ball.pos[0], currentState_.ball.pos[1], currentState_.ball.pos[2]},
                       tickCounter_, {currentState_.score[0], currentState_.score[1]}};
        eventQueue_.push_back(ev);
        stats_.goals[team]++;
        stats_.shots[team]++;
    }
}

void Server::postMatchResult(const std::string& statsUrl) {
    stats_.duration_s = cfg_.duration;
    stats_.score[0] = currentState_.score[0];
    stats_.score[1] = currentState_.score[1];

    float total = stats_.possession_ticks[0] + stats_.possession_ticks[1];
    if (total > 0) {
        stats_.possession_ticks[0] = (stats_.possession_ticks[0] / total) * 100.0f;
        stats_.possession_ticks[1] = (stats_.possession_ticks[1] / total) * 100.0f;
    }

    // TODO: HTTP POST JSON to Stats Service /internal/match-result using libcurl
    std::cout << "[GameServer] Match finished. Score: "
              << stats_.score[0] << " - " << stats_.score[1]
              << " | Possession: " << stats_.possession_ticks[0] << "% - " << stats_.possession_ticks[1] << "%"
              << std::endl;
}

} // namespace GameServer
