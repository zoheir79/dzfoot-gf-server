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

class GameEnv;
class RedisClient;

namespace GameServer {

// ------------------------------------------------------------------
// Animation IDs sent to client (must match glTF animation names)
// Client maps these IDs to the animation clips exported by blunted2_gltf_exporter.
// ------------------------------------------------------------------
// Animation IDs — aligned with ROADMAP AnimationStateSelector.
// Client maps these IDs to glTF animation clips exported by blunted2_gltf_exporter.
enum AnimId : uint8_t {
    ANIM_IDLE      = 0,
    ANIM_WALK      = 1,
    ANIM_RUN       = 2,
    ANIM_SPRINT    = 3,
    ANIM_SHOOT_R   = 4,   // shoot with right foot
    ANIM_SHOOT_L   = 5,   // shoot with left foot
    ANIM_PASS_S    = 6,   // short pass
    ANIM_PASS_L    = 7,   // long / high pass
    ANIM_HEADER    = 8,
    ANIM_TACKLE    = 9,   // sliding tackle
    ANIM_DRIBBLE   = 10,
    ANIM_FALL      = 11,  // trip / fall
    ANIM_CELEBRATE = 12,
    ANIM_GK_IDLE   = 13,
    ANIM_GK_DIVE_L = 14,  // keeper dive left
    ANIM_GK_DIVE_R = 15,  // keeper dive right
    ANIM_GK_CATCH  = 16,
    ANIM_COUNT
};

// ------------------------------------------------------------------
// Player input from Android client (topic "in")
// ------------------------------------------------------------------
// Player input packet from Android client (topic "in")
// Layout: dirX(4), dirZ(4), kick(4), pass(4), highPass(4), shot(4), sliding(4),
//         dribble(4), sprint(4), switchPlayer(4), playerIdx(1), team(1), pad(2),
//         clientTick(4), clientTimeUs(8) = 56 bytes
struct PlayerInput {
    float    dirX = 0.0f;
    float    dirZ = 0.0f;
    bool     kick = false;       // long pass
    bool     pass = false;       // short pass
    bool     highPass = false;   // high/lob pass
    bool     shot = false;
    bool     sliding = false;    // tackle slide
    bool     dribble = false;
    bool     sprint = false;
    bool     switchPlayer = false; // force switch active player (FIFA-style)
    uint8_t  playerIdx = 0;      // 0..10 within team
    uint8_t  team = 0;           // 0 = left, 1 = right
    uint32_t clientTick = 0;   // anti-lag: client local tick
    uint64_t clientTimeUs = 0; // anti-lag: client timestamp
};

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
    int broadcastRateHz = 20; // GameState broadcast rate (simulation stays at 60 Hz)
    uint8_t gameMode = 1; // 0=1v1 (both human), 1=vs AI (A human, B AI)
    std::atomic<bool>* shutdownFlag = nullptr; // graceful shutdown from signal handler
    std::string matchConfigPath; // JSON file with formation, duration, etc. (optional)
    std::string matchConfigJson; // JSON string with formation, duration, etc. (optional)
};

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
// Compact runtime GameState broadcast at 20 Hz (topic "gs", unreliable)
// Total size: ~520 bytes binary -> fits in a single datagram
// ------------------------------------------------------------------
struct GameState {
    // Header
    uint32_t tick = 0;
    uint64_t timestampUs = 0; // server time for client reconciliation
    uint8_t  gameMode = 0;    // e_GameMode
    uint8_t  flags = 0;       // bit0=is_in_play, bit1=goal_scored_this_tick
    uint8_t  score[2] = {};
    float    timer = 0.0f;    // match time in seconds

    struct BallState {
        float pos[3] = {};
        float vel[3] = {};   // needed for dead reckoning on client
        float rot[3] = {};   // angular velocity / visual rotation
        int8_t ownedTeam = -1;
        int8_t ownedPlayer = -1;
    } ball;

    struct PlayerState {
        float pos[3] = {};
        float vel[3] = {};
        float dir[3] = {};   // forward direction vector (normalized)
        float rotY = 0.0f;   // heading angle around Y axis (radians)
        uint8_t anim = 0;    // AnimId
        uint8_t team = 0;
        uint8_t role = 0;    // e_PlayerRole
        uint8_t flags = 0;   // bit0=is_active, bit1=has_card, bit2=designated_player, bit3=has_possession
        float tiredFactor = 0.0f; // [0..1]
    };
    PlayerState players[kMaxPlayers];
};
static_assert(sizeof(GameState) < 1200, "GameState should fit in a single datagram (<1200 bytes)");

// ------------------------------------------------------------------
// Match events (topic "ev", reliable)
// ------------------------------------------------------------------
enum EventType : uint8_t {
    EVENT_GOAL = 0,
    EVENT_YELLOW_CARD = 1,
    EVENT_RED_CARD = 2,
    EVENT_SUBSTITUTION = 3,
    EVENT_CORNER = 4,
    EVENT_THROW_IN = 5,
    EVENT_FREE_KICK = 6,
    EVENT_PENALTY = 7,
    EVENT_KICK_OFF = 8,
    EVENT_END_MATCH = 9,
    EVENT_HALF_TIME = 10
};

struct MatchEvent {
    uint8_t  event_type = 0;
    uint8_t  team = 0;
    uint8_t  player_idx = 0;
    float    pos[3] = {};
    uint32_t tick = 0;
    uint8_t  score[2] = {};
    uint8_t  extra = 0; // event-specific (e.g. card count, half number)
};

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
    void receiveInput(const PlayerInput& input);

    // Build and broadcast static player info once (glTF compatibility)
    void sendMatchSetup();

private:
    void tick();
    void broadcastGameState();
    void processInputs();
    void applyPendingInputs();
    void accumulateStats();
    void detectEvents();

    // Animation deduction from GF internal state
    uint8_t deduceAnimId(int playerIndex, int teamId);

    // Helpers
    float getHeadingFromDir(float dx, float dz) const;

    Config cfg_;
    MatchStats stats_;
    std::atomic<bool> running_{true};
    GameState currentState_;
    GameState previousState_; // for velocity diff
    uint32_t tickCounter_ = 0;
    std::vector<MatchEvent> eventQueue_;
    std::chrono::steady_clock::time_point startTime_;
    uint64_t baseTimestampUs_ = 0;

    GameEnv* gameEnv_ = nullptr;
    std::mutex inputMutex_;
    std::queue<PlayerInput> inputQueue_;

    // Events tracking (score changes, etc.)
    int lastScoreA_ = 0;
    int lastScoreB_ = 0;
    bool matchSetupSent_ = false;
    bool halfTimeSent_ = false;

    // Per-player tracking for stat detection (shots/passes/tackles).
    // Index = teamId * 11 + playerIndex (matches GameState::players layout).
    uint8_t prevFunctionType_[kMaxPlayers] = {};
    int     pendingShotExpiryTick_[kMaxPlayers] = {}; // 0 = no pending shot
    int     pendingPassExpiryTick_[kMaxPlayers] = {}; // 0 = no pending pass
    int     pendingPassPlayerIdx_[kMaxPlayers] = {};  // who started the pass
};

} // namespace GameServer
