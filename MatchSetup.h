#pragma once
#include <cstdint>
#include <string>
#include <array>

namespace GameServer {

// Number of PlayerStat entries (see utils.hpp PlayerStat enum)
static constexpr int kNumPlayerStats = 21;
static constexpr int kMaxPlayers = 22;
static constexpr int kMaxNameLen = 32;
static constexpr int kMaxHairLen = 16;

// Static player data sent once at match start.
// Client needs this to select correct glTF model/skin and show stats in UI.
struct PlayerStaticInfo {
    uint8_t  index;              // 0..21
    uint8_t  team;               // 0 or 1
    uint8_t  role;               // e_PlayerRole
    char     lastName[kMaxNameLen];
    float    height;             // meters
    int      skinColor;          // 0..N
    char     hairStyle[kMaxHairLen];
    char     hairColor[kMaxHairLen];
    float    stats[kNumPlayerStats]; // physical/technical/mental (see PlayerStat enum)
};

struct MatchSetupPacket {
    uint32_t magic = 0x445A5345; // 'DZSE' (DZFoot Setup Endian check)
    uint32_t version = 1;
    uint8_t  playerCount = kMaxPlayers;
    char     teamAName[kMaxNameLen];
    char     teamBName[kMaxNameLen];
    uint8_t  stadiumId;
    uint8_t  durationMinutes;
    PlayerStaticInfo players[kMaxPlayers];
};

} // namespace GameServer
