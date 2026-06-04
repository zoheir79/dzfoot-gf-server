#include "MatchConfig.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>

using json = nlohmann::json;

namespace GameServer {

e_PlayerRole parseRole(const std::string& role) {
    if (role == "GK") return e_PlayerRole_GK;
    if (role == "CB") return e_PlayerRole_CB;
    if (role == "LB") return e_PlayerRole_LB;
    if (role == "RB") return e_PlayerRole_RB;
    if (role == "DM") return e_PlayerRole_DM;
    if (role == "CM") return e_PlayerRole_CM;
    if (role == "LM") return e_PlayerRole_LM;
    if (role == "RM") return e_PlayerRole_RM;
    if (role == "AM") return e_PlayerRole_AM;
    if (role == "CF") return e_PlayerRole_CF;
    std::cerr << "[MatchConfig] Unknown role '" << role << "', defaulting to CM" << std::endl;
    return e_PlayerRole_CM;
}

std::vector<FormationEntry> buildFormation(const TeamConfig& tc) {
    std::vector<FormationEntry> out;
    out.reserve(tc.formation.size());
    for (const auto& e : tc.formation) {
        // Do NOT use FormationEntry(x,y,...) constructor because it
        // multiplies y by FORMATION_Y_SCALE=-2.36. Set raw coordinates directly.
        FormationEntry fe;
        fe.position = Vector3(e.x, e.y, 0);
        fe.start_position = fe.position;
        fe.role = parseRole(e.role);
        fe.lazy = false;
        fe.controllable = e.controllable;
        out.push_back(fe);
    }
    return out;
}

bool MatchConfig::loadString(const std::string& jsonStr, MatchConfig& out) {
    try {
        json j = json::parse(jsonStr);

        out.duration_seconds = j.value("duration_seconds", 600);
        out.mode = j.value("mode", "vs_ai");

        auto parseTeam = [&](const json& teamJ, TeamConfig& tc) {
            tc.name = teamJ.value("name", "default");
            if (teamJ.contains("formation")) {
                for (const auto& p : teamJ["formation"]) {
                    TeamFormationEntry e;
                    e.role = p.value("role", "CM");
                    e.x = p.value("x", 0.0f);
                    e.y = p.value("y", 0.0f);
                    e.controllable = p.value("controllable", true);
                    tc.formation.push_back(e);
                }
            }
            if (teamJ.contains("players")) {
                for (const auto& pj : teamJ["players"]) {
                    PlayerSkillProfile ps;
                    ps.name = pj.value("name", "Player");
                    ps.position = pj.value("position", "CM");
                    ps.number = pj.value("number", 0);
                    if (pj.contains("skills") && pj["skills"].is_object()) {
                        for (auto& [k, v] : pj["skills"].items()) {
                            ps.skills[k] = v.get<float>();
                        }
                    }
                    tc.players.push_back(std::move(ps));
                }
            }
        };

        if (j.contains("left_team"))  parseTeam(j["left_team"],  out.left_team);
        if (j.contains("right_team")) parseTeam(j["right_team"], out.right_team);

        return true;
    } catch (const std::exception& e) {
        std::cerr << "[MatchConfig] JSON parse error: " << e.what() << std::endl;
        return false;
    }
}

bool MatchConfig::load(const std::string& path, MatchConfig& out) {
    std::ifstream f(path);
    if (!f.is_open()) {
        std::cerr << "[MatchConfig] Cannot open " << path << std::endl;
        return false;
    }
    std::string str((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());
    return loadString(str, out);
}

} // namespace GameServer
