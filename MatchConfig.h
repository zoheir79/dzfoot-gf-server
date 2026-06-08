#pragma once
#include <string>
#include <map>
#include <vector>
#include "gamedefines.hpp"

namespace GameServer {

// ------------------------------------------------------------------
// Player skills (22 entries matching GF PlayerStat enum)
// ------------------------------------------------------------------
struct PlayerSkillProfile {
    std::string name;
    std::string position;  // "GK", "CB", "LW", etc.
    int number = 0;
    std::map<std::string, float> skills;  // key: "physical_velocity", value: 0.85

    // Custom deterministic avatar fields populated by backend
    int skinColor = 3;
    std::string hairStyle = "short";
    std::string hairColor = "black";
    float height = 1.78f;
    int bodyType = 1;
    int beardStyle = 0;
    int eyeColor = 0;
};

struct TeamFormationEntry {
    std::string role;      // "GK", "CB", "LB", "RB", "DM", "CM", "LM", "RM", "AM", "CF"
    float x = 0.0f;
    float y = 0.0f;
    bool controllable = true;
};

struct TeamConfig {
    std::string name;
    std::vector<TeamFormationEntry> formation;  // 11 entries expected
    std::vector<PlayerSkillProfile> players;    // 11 entries, same order as formation
};

struct MatchConfig {
    int duration_seconds = 600;
    std::string mode = "vs_ai";  // "1v1" or "vs_ai"
    std::string stadium_id;        // e.g. "stade-5-juillet"
    TeamConfig left_team;
    TeamConfig right_team;

    // Load from JSON file (returns true on success)
    static bool load(const std::string& path, MatchConfig& out);

    // Load from JSON string (returns true on success)
    static bool loadString(const std::string& jsonStr, MatchConfig& out);
};

// Convert string role to GF e_PlayerRole
e_PlayerRole parseRole(const std::string& role);

// Build GF FormationEntry vector from TeamConfig
std::vector<FormationEntry> buildFormation(const TeamConfig& tc);

} // namespace GameServer
