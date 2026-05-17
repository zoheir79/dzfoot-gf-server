#pragma once
#include <atomic>
#include <string>
#include <cstdint>
#include <vector>
#include <chrono>

class LiveKitBridge;

namespace GameServer {

struct Config {
    std::string roomId;
    std::string teamA;
    std::string teamB;
    std::string stadium;
    int duration;
    std::string statsUrl;
    std::string redisUrl;  // For heartbeat and ready signals
    LiveKitBridge* livekitBridge = nullptr;
};

struct MatchStats {
    int goals[2] = {};
    int shots[2] = {};
    int shots_on_target[2] = {};
    int passes[2] = {};
    int passes_success[2] = {};
    int tackles[2] = {};
    int yellow_cards[2] = {};
    int red_cards[2] = {};
    float possession_ticks[2] = {};
    float distance_m[2] = {};
    int score[2] = {};
    int duration_s = 0;
};

struct GameState {
    struct PlayerState {
        float pos[3];
        float vel[3];
        float rot;
        uint8_t anim;
        uint8_t team;
    };
    struct BallState {
        float pos[3];
        float vel[3];
    };
    PlayerState players[22];
    BallState ball;
    int score[2];
    float timer;
    uint8_t gameMode;
    uint32_t tick;
};

struct MatchEvent {
    uint8_t event_type;
    uint8_t team;
    uint8_t player_idx;
    float pos[3];
    uint32_t tick;
    int score[2];
};

class Server {
public:
    Server(const Config& cfg);
    void run();
    void postMatchResult(const std::string& statsUrl);

private:
    void tick();
    void broadcastGameState();
    void processInputs();
    void accumulateStats();
    void detectEvents();
    uint8_t deduceAnimId(const GameState::PlayerState& p, const GameState::BallState& ball);

    Config cfg_;
    MatchStats stats_;
    std::atomic<bool> running_{true};
    GameState currentState_;
    uint32_t tickCounter_ = 0;
    std::vector<MatchEvent> eventQueue_;
    std::chrono::steady_clock::time_point startTime_;
};

} // namespace GameServer
