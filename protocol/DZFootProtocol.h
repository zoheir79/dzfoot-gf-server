#pragma once
#include <cstdint>
#include <cstddef>
#include <type_traits>
#include <cmath>

// ============================================================================
// DZFoot Shared Binary Protocol v1
// Source of truth: dzfoot-gf-server/protocol/DZFootProtocol.h
// Sync copy    : dzfoot-android/src/main/cpp/protocol/DZFootProtocol.h
// Endianness   : Little-endian (both x86_64 Linux server and ARM64 Android)
// Rules        : No bool, no int, no pointers, no std::string/std::vector
// ============================================================================

namespace dzfoot {

constexpr uint32_t DZ_MAGIC           = 0x54465A44; // 'DZFT' little-endian
constexpr uint16_t DZ_PROTOCOL_VERSION = 1;
constexpr uint8_t  DZ_MAX_PLAYERS      = 22;

// ------------------------------------------------------------------
// Packet header (present in every packet)
// ------------------------------------------------------------------
enum PacketType : uint16_t {
    PACKET_GAME_STATE   = 1,
    PACKET_MATCH_EVENT  = 2,
    PACKET_PLAYER_INPUT = 3,
    PACKET_MATCH_SETUP  = 4
};

struct PacketHeader {
    uint32_t magic;      // DZ_MAGIC ('DZFT')
    uint16_t version;    // DZ_PROTOCOL_VERSION
    uint16_t type;       // PacketType
    uint16_t size;       // sizeof(entire packet)
    uint16_t flags;      // reserved (0)
};
static_assert(sizeof(PacketHeader) == 12, "PacketHeader size must be exactly 12 bytes");
static_assert(std::is_standard_layout<PacketHeader>::value, "PacketHeader must be standard layout");

// ------------------------------------------------------------------
// Animation IDs (shared server + client)
// ------------------------------------------------------------------
enum AnimId : uint8_t {
    ANIM_IDLE      = 0,
    ANIM_WALK      = 1,
    ANIM_RUN       = 2,
    ANIM_SPRINT    = 3,
    ANIM_SHOOT_R   = 4,
    ANIM_SHOOT_L   = 5,
    ANIM_PASS_S    = 6,
    ANIM_PASS_L    = 7,
    ANIM_HEADER    = 8,
    ANIM_TACKLE    = 9,
    ANIM_DRIBBLE   = 10,
    ANIM_FALL      = 11,
    ANIM_CELEBRATE = 12,
    ANIM_GK_IDLE   = 13,
    ANIM_GK_DIVE_L = 14,
    ANIM_GK_DIVE_R = 15,
    ANIM_GK_CATCH  = 16,
    ANIM_COUNT
};

// ------------------------------------------------------------------
// Network state structs (packed tightly, no padding surprises)
// ------------------------------------------------------------------
#pragma pack(push, 1)

struct NetworkBallState {
    float  pos[3];
    float  vel[3];
    float  rot[3];
    int8_t ownedTeam;
    int8_t ownedPlayer;
    uint8_t _pad[2];
};
static_assert(sizeof(NetworkBallState) == 40, "NetworkBallState size mismatch");

struct NetworkPlayerState {
    float  pos[3];
    float  vel[3];
    float  dir[3];
    float  rotY;
    uint8_t anim;
    uint8_t team;
    uint8_t role;
    uint8_t flags;
    float  tiredFactor;
};
static_assert(sizeof(NetworkPlayerState) == 48, "NetworkPlayerState size mismatch");

// ------------------------------------------------------------------
// GameState packet (topic "gs", unreliable)
// ------------------------------------------------------------------
struct GameStatePacket {
    PacketHeader header;
    uint32_t tick;
    uint64_t timestampUs;
    uint8_t  gameMode;
    uint8_t  gameFlags;
    uint8_t  score[2];
    float    timer;
    NetworkBallState ball;
    NetworkPlayerState players[DZ_MAX_PLAYERS];
};
static_assert(sizeof(GameStatePacket) == 12 + 4 + 8 + 1 + 1 + 2 + 4 + 40 + (48 * 22), "GameStatePacket size sanity check");
static_assert(sizeof(GameStatePacket) < 1200, "GameStatePacket must fit in a single datagram");

// ------------------------------------------------------------------
// MatchEvent packet (topic "ev", reliable)
// ------------------------------------------------------------------
struct MatchEventPacket {
    PacketHeader header;
    uint8_t  eventType;   // EventType enum
    uint8_t  team;
    uint8_t  playerIdx;
    uint8_t  extra;
    float    pos[3];
    uint32_t tick;
    uint8_t  score[2];
    uint8_t  _pad[2];
};
static_assert(sizeof(MatchEventPacket) == 12 + 1 + 1 + 1 + 1 + 12 + 4 + 2 + 2, "MatchEventPacket size sanity check");

// ------------------------------------------------------------------
// PlayerInput packet (topic "in", reliable or unreliable)
// ------------------------------------------------------------------
struct PlayerInputPacket {
    PacketHeader header;
    float    dirX;
    float    dirZ;
    uint16_t buttons;     // bitmask: see BUTTON_* below
    uint8_t  playerIdx;   // 0..10
    uint8_t  team;        // 0 or 1
    uint32_t clientTick;  // anti-lag: local tick
    uint64_t clientTimeUs;// anti-lag: local timestamp
};
static_assert(sizeof(PlayerInputPacket) == 12 + 4 + 4 + 2 + 1 + 1 + 4 + 8, "PlayerInputPacket size sanity check");

// ------------------------------------------------------------------
// EventType enum (for MatchEventPacket.eventType)
// ------------------------------------------------------------------
enum EventType : uint8_t {
    EVENT_GOAL         = 0,
    EVENT_YELLOW_CARD  = 1,
    EVENT_RED_CARD     = 2,
    EVENT_SUBSTITUTION = 3,
    EVENT_CORNER       = 4,
    EVENT_THROW_IN     = 5,
    EVENT_FREE_KICK    = 6,
    EVENT_PENALTY      = 7,
    EVENT_KICK_OFF     = 8,
    EVENT_END_MATCH    = 9,
    EVENT_HALF_TIME    = 10
};

// ------------------------------------------------------------------
// Button bitmask for PlayerInputPacket.buttons
// ------------------------------------------------------------------
constexpr uint16_t BUTTON_KICK          = 1 << 0;  // long pass
constexpr uint16_t BUTTON_PASS            = 1 << 1;  // short pass
constexpr uint16_t BUTTON_HIGH_PASS       = 1 << 2;  // high/lob pass
constexpr uint16_t BUTTON_SHOT            = 1 << 3;
constexpr uint16_t BUTTON_SLIDING       = 1 << 4;  // tackle
constexpr uint16_t BUTTON_DRIBBLE       = 1 << 5;
constexpr uint16_t BUTTON_SPRINT        = 1 << 6;
constexpr uint16_t BUTTON_SWITCH_PLAYER = 1 << 7;

// ------------------------------------------------------------------
// MatchSetup packet (sent once at startup, topic "ev" reliable)
// ------------------------------------------------------------------
constexpr uint8_t kNumPlayerStats = 21;
constexpr uint8_t kMaxNameLen     = 32;
constexpr uint8_t kMaxHairLen     = 16;

struct PlayerStaticInfo {
    uint8_t  index;              // 0..21
    uint8_t  team;               // 0 or 1
    uint8_t  role;               // e_PlayerRole
    char     lastName[kMaxNameLen];
    float    height;             // meters
    int32_t  skinColor;          // 0..N
    char     hairStyle[kMaxHairLen];
    char     hairColor[kMaxHairLen];
    float    stats[kNumPlayerStats]; // physical/technical/mental
};
static_assert(sizeof(PlayerStaticInfo) == 3 + kMaxNameLen + 4 + 4 + kMaxHairLen * 2 + (4 * kNumPlayerStats), "PlayerStaticInfo size check");

struct MatchSetupPacket {
    PacketHeader header;
    uint8_t  playerCount;        // usually 22
    char     teamAName[kMaxNameLen];
    char     teamBName[kMaxNameLen];
    uint8_t  stadiumId;
    uint8_t  durationMinutes;
    PlayerStaticInfo players[DZ_MAX_PLAYERS];
};
static_assert(sizeof(MatchSetupPacket) == 12 + 1 + kMaxNameLen * 2 + 1 + 1 + (sizeof(PlayerStaticInfo) * DZ_MAX_PLAYERS), "MatchSetupPacket size check");

#pragma pack(pop)

// ============================================================================
// Validation helpers (use before casting raw bytes to structs)
// ============================================================================

inline bool validatePacketHeader(const uint8_t* data, size_t len, uint16_t expectedType, size_t expectedSize) {
    if (len < sizeof(PacketHeader)) return false;
    const PacketHeader* h = reinterpret_cast<const PacketHeader*>(data);
    if (h->magic != DZ_MAGIC) return false;
    if (h->version != DZ_PROTOCOL_VERSION) return false;
    if (h->type != expectedType) return false;
    if (h->size != expectedSize) return false;
    if (len < expectedSize) return false;
    return true;
}

inline bool validateGameStatePacket(const uint8_t* data, size_t len) {
    return validatePacketHeader(data, len, PACKET_GAME_STATE, sizeof(GameStatePacket));
}

inline bool validateMatchEventPacket(const uint8_t* data, size_t len) {
    return validatePacketHeader(data, len, PACKET_MATCH_EVENT, sizeof(MatchEventPacket));
}

inline bool validatePlayerInputPacket(const uint8_t* data, size_t len) {
    return validatePacketHeader(data, len, PACKET_PLAYER_INPUT, sizeof(PlayerInputPacket));
}

inline bool validateMatchSetupPacket(const uint8_t* data, size_t len) {
    return validatePacketHeader(data, len, PACKET_MATCH_SETUP, sizeof(MatchSetupPacket));
}

// ============================================================================
// Sanitization helpers (use after deserialization)
// ============================================================================

inline bool isFiniteVec3(const float* v) {
    return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}

inline bool sanitizePlayerInput(PlayerInputPacket& in) {
    if (!std::isfinite(in.dirX) || !std::isfinite(in.dirZ)) return false;
    // clamp direction
    if (in.dirX < -1.0f) in.dirX = -1.0f;
    if (in.dirX >  1.0f) in.dirX =  1.0f;
    if (in.dirZ < -1.0f) in.dirZ = -1.0f;
    if (in.dirZ >  1.0f) in.dirZ =  1.0f;
    // normalize if length > 1
    float lenSq = in.dirX * in.dirX + in.dirZ * in.dirZ;
    if (lenSq > 1.0f) {
        float invLen = 1.0f / std::sqrt(lenSq);
        in.dirX *= invLen;
        in.dirZ *= invLen;
    }
    if (in.team > 1) return false;
    if (in.playerIdx > 10) return false;
    return true;
}

} // namespace dzfoot
