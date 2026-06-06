#pragma once
#include <cstdint>
#include <string>
#include <array>

#include "protocol/DZFootProtocol.h"

namespace GameServer {

// Re-export protocol constants for backward compatibility
using dzfoot::kNumPlayerStats;
using dzfoot::DZ_MAX_PLAYERS;
using dzfoot::kMaxNameLen;
using dzfoot::kMaxHairLen;
static constexpr int kMaxPlayers = DZ_MAX_PLAYERS; // legacy alias

// Re-export protocol types so GameServer code uses the canonical definitions
using dzfoot::PlayerStaticInfo;
using dzfoot::MatchSetupPacket;

} // namespace GameServer
