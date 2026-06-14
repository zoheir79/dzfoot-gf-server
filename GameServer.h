#pragma once
#include <atomic>
#include <string>
#include <cstdint>
#include <vector>
#include <chrono>
#include <queue>
#include <mutex>
#include <array>
#include <memory>
#include "MatchSetup.h"
#include "protocol/DZFootProtocol.h"

class GameEnv;
class RedisClient;
class Player;
class Team;
class TeamAIController;

namespace GameServer {

// Use shared protocol types
using dzfoot::AnimId;
using dzfoot::PlayerInputPacket;
using dzfoot::GameStatePacket;
using dzfoot::MatchEventPacket;
using dzfoot::TacticalStatePacket;
using dzfoot::EventType;
using dzfoot::NetworkBallState;
using dzfoot::NetworkPlayerState;
using dzfoot::DZ_MAX_PLAYERS;
using dzfoot::ANIM_IDLE;
using dzfoot::ANIM_WALK;
using dzfoot::ANIM_RUN;
using dzfoot::ANIM_SPRINT;
using dzfoot::ANIM_SHOOT_R;
using dzfoot::ANIM_SHOOT_L;
using dzfoot::ANIM_PASS_S;
using dzfoot::ANIM_PASS_L;
using dzfoot::ANIM_HEADER;
using dzfoot::ANIM_TACKLE;
using dzfoot::ANIM_DRIBBLE;
using dzfoot::ANIM_FALL;
using dzfoot::ANIM_CELEBRATE;
using dzfoot::ANIM_GK_IDLE;
using dzfoot::ANIM_GK_DIVE_L;
using dzfoot::ANIM_GK_DIVE_R;
using dzfoot::ANIM_GK_CATCH;

// Backward-compat alias (deprecated, use PlayerInputPacket directly)
using PlayerInput = PlayerInputPacket;

// ------------------------------------------------------------------
// Server configuration
// ------------------------------------------------------------------
struct Config {
    std::string roomId;
    std::string teamA;
    std::string teamB;
    std::string stadium;
    std::string playerA; // player UUID (team A)
    std::string playerB; // player UUID (team B)
    int duration = 600; // seconds
    std::string statsUrl;
    std::string redisUrl;
    RedisClient* redis = nullptr; // for publishing game states + subscribing inputs
    int broadcastRateHz = 20; // GameState broadcast rate (simulation at 100 Hz, state at 60 Hz)
    uint8_t gameMode = 1; // 0=1v1 (both human), 1=vs AI (A human, B AI)
    std::atomic<bool>* shutdownFlag = nullptr; // graceful shutdown from signal handler
    std::string matchConfigPath; // JSON file with formation, duration, etc. (optional)
    std::string matchConfigJson; // JSON string with formation, duration, etc. (optional)
};

// ------------------------------------------------------------------
// Legacy compact GameState broadcast (kept for reference; use GameStatePacket)
// ------------------------------------------------------------------
using GameState = GameStatePacket;

// ------------------------------------------------------------------
// Accumulated match statistics (sent at end of match)
// ------------------------------------------------------------------
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

// ------------------------------------------------------------------
// Legacy MatchEvent alias (use MatchEventPacket)
// ------------------------------------------------------------------
using MatchEvent = MatchEventPacket;

// ------------------------------------------------------------------
// Main server class
// ------------------------------------------------------------------
class Server {
public:
    explicit Server(const Config& cfg);
    ~Server();
    void run();
    void stop() { running_ = false; }
    void postMatchResult(const std::string& statsUrl);
    void receiveInput(const PlayerInputPacket& input);

    // Build and broadcast static player info once (glTF compatibility)
    void sendMatchSetup();

private:
    void updateState();
    void broadcastGameState();
    void updateTacticalState();
    void broadcastTacticalState();
    void processInputs();
    void applyPendingInputs();
    void accumulateStats();
    void detectEvents();

    // Animation deduction from GF internal state
    uint8_t deduceAnimId(int playerIndex, int teamId);
    uint8_t deduceAiIntent(::Player* player, ::TeamAIController* controller, int playerIndex, int teamId);
    int findPlayerIndex(::Team* team, ::Player* player) const;
    int findPlayerIndex(::Team* team, int playerID) const;

    // Helpers
    float getHeadingFromDir(float dx, float dz) const;

    Config cfg_;
    MatchStats stats_;
    std::atomic<bool> running_{true};
    GameState currentState_;
    GameState previousState_; // for velocity diff
    TacticalStatePacket tacticalState_ = {};
    uint32_t tickCounter_ = 0;
    std::vector<MatchEvent> eventQueue_;
    std::chrono::steady_clock::time_point startTime_;
    uint64_t baseTimestampUs_ = 0;

    GameEnv* gameEnv_ = nullptr;
    std::mutex inputMutex_;
    std::queue<PlayerInputPacket> inputQueue_;

    // Events tracking (score changes, etc.)
    int lastScoreA_ = 0;
    int lastScoreB_ = 0;
    bool matchSetupSent_ = false;
    bool halfTimeSent_ = false;
    uint8_t lastGameMode_ = 255;
    std::string stadiumId_;  // from match config JSON

    // Per-player tracking for stat detection (shots/passes/tackles).
    // Index = teamId * 11 + playerIndex (matches GameState::players layout).
    uint8_t prevFunctionType_[kMaxPlayers] = {};
    int     pendingShotExpiryTick_[kMaxPlayers] = {}; // 0 = no pending shot
    int     pendingPassExpiryTick_[kMaxPlayers] = {}; // 0 = no pending pass
    int     pendingPassPlayerIdx_[kMaxPlayers] = {};  // who started the pass

    // Referee event tracking
    int     lastRefereeFoulType_ = 0;   // 0=none, 1=foul, 2=yellow, 3=red
    int     lastRefereeFoulPlayer_ = -1;
    bool    wasOffside_ = false;
    int     lastOffsideTick_ = 0;

    // Goal celebration: force ANIM_CELEBRATE on scorer for ~3 seconds
    int     celebrationExpiryTick_[kMaxPlayers] = {};

// NOTE: LiveKit publishing is done by the backend Python bot, not by gf_server.
// gf_server only uses Redis for all network I/O.

    // Jersey numbers from match config JSON (index = team*11 + player)
    uint8_t playerNumbers_[DZ_MAX_PLAYERS] = {};
    uint8_t bodyTypes_[DZ_MAX_PLAYERS] = {};
    uint8_t beardStyles_[DZ_MAX_PLAYERS] = {};
    uint8_t eyeColors_[DZ_MAX_PLAYERS] = {};
};

} // namespace GameServer
