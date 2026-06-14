#include "GameServer.h"
#include "RedisClient.h"
#include "StatsPoster.h"
#include "MatchConfig.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <cstring>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <unistd.h>
#include <algorithm>

#include "game_env.hpp"
#include "gfootball_actions.h"
#include "gamedefines.hpp"
#include "timestep_config.hpp"

// GF internal headers for deep state extraction
#include "main.hpp"
#include "onthepitch/match.hpp"
#include "onthepitch/team.hpp"
#include "onthepitch/ball.hpp"
#include "onthepitch/player/player.hpp"
#include "onthepitch/player/playerbase.hpp"
#include "onthepitch/player/humanoid/humanoid.hpp"
#include "onthepitch/referee.hpp"
#include "onthepitch/officials.hpp"
#include "onthepitch/player/playerofficial.hpp"
#include "data/playerdata.hpp"
#include "data/teamdata.hpp"
#include "utils.hpp"

namespace GameServer {

using namespace dzfoot;

// Forward declaration: defined at end of this namespace
std::string skillNameToEnum(const std::string& name);

// ------------------------------------------------------------------
// Helpers
// ------------------------------------------------------------------
static void copyString(char* dst, size_t dstLen, const std::string& src) {
    if (dstLen == 0) return;
    std::strncpy(dst, src.c_str(), dstLen - 1);
    dst[dstLen - 1] = '\0';
}

// Default 4-3-3 formation matching GameplayFootball/data/teamdata.cpp default XML.
// Used to populate ScenarioConfig::{left,right}_team before start_game() so that
// Match::GetState() can resize SharedInfo controller vectors safely.
static void appendDefault433(std::vector<FormationEntry>& dst, bool controllable, bool mirror) {
    // Raw formation coordinates (same as teamdata.cpp XML parsing).
    // We must NOT use FormationEntry(x,y,...) constructor because it
    // multiplies y by FORMATION_Y_SCALE=-2.36, but teamdata.cpp overwrites
    // the 'position' member directly with raw XML vectors when f is empty.
    // Using the constructor causes positions to be 2.36x too large in Y,
    // pushing wingers (LB/RB/LM/RM) outside pitchHalfH=36 at kickoff.
    const float sx = mirror ? -1.0f : 1.0f;
    const float sy = mirror ? -1.0f : 1.0f;
    auto add = [&](float x, float y, e_PlayerRole role) {
        FormationEntry fe;
        fe.position = Vector3(sx * x, sy * y, 0);
        fe.start_position = fe.position;
        fe.role = role;
        fe.lazy = false;
        fe.controllable = controllable;
        dst.push_back(fe);
    };
    add(-1.0f,  0.00f, e_PlayerRole_GK);
    add(-0.7f,  0.75f, e_PlayerRole_LB);
    add(-1.0f,  0.25f, e_PlayerRole_CB);
    add(-1.0f, -0.25f, e_PlayerRole_CB);
    add(-0.7f, -0.75f, e_PlayerRole_RB);
    add( 0.0f,  0.50f, e_PlayerRole_CM);
    add(-0.2f,  0.00f, e_PlayerRole_CM);
    add( 0.0f, -0.50f, e_PlayerRole_CM);
    add( 0.6f,  0.75f, e_PlayerRole_LM);
    add( 1.0f,  0.00f, e_PlayerRole_CF);
    add( 0.6f, -0.75f, e_PlayerRole_RM);
}

// ------------------------------------------------------------------
// Server
// ------------------------------------------------------------------
Server::Server(const Config& cfg) : cfg_(cfg) {
    std::memset(&currentState_, 0, sizeof(currentState_));
    std::memset(&previousState_, 0, sizeof(previousState_));
    currentState_.timer = 0.0f;
    currentState_.tick = 0;
    currentState_.gameMode = 0;
    currentState_.score[0] = 0;
    currentState_.score[1] = 0;
    startTime_ = std::chrono::steady_clock::now();
}

// Defined here (not in header) so unique_ptr<RedisClient> can see complete type.
Server::~Server() {
    // gf_server uses Redis only — no direct LiveKit connection.
}

void Server::sendMatchSetup() {
    bool hasRedis = cfg_.redis && cfg_.redis->isConfigured();
    if (!gameEnv_ || !hasRedis) return;

    MatchSetupPacket setup{};
    setup.header.magic = dzfoot::DZ_MAGIC;
    setup.header.version = dzfoot::DZ_PROTOCOL_VERSION;
    setup.header.type = dzfoot::PACKET_MATCH_SETUP;
    setup.header.size = sizeof(MatchSetupPacket);
    setup.header.flags = 0;
    copyString(setup.teamAName, sizeof(setup.teamAName), cfg_.teamA);
    copyString(setup.teamBName, sizeof(setup.teamBName), cfg_.teamB);
    setup.durationMinutes = static_cast<uint8_t>(cfg_.duration / 60);
    {
        // Simple string hash (uint8_t) — 256 possible values. Collisions are
        // extremely unlikely for the small set of stadium names DZFoot uses.
        uint8_t hash = 0;
        for (char c : stadiumId_) hash = hash * 31 + static_cast<uint8_t>(c);
        setup.stadiumId = hash;
    }

    Match* match = gameEnv_->context->gameTask->GetMatch();
    if (!match) {
        std::cerr << "[GameServer] sendMatchSetup: match not available" << std::endl;
        return;
    }

    // Team colors
    for (int t = 0; t < 2; ++t) {
        Team* team = match->GetTeam(t);
        if (!team) continue;
        TeamData* td = team->GetTeamData();
        if (!td) continue;
        Vector3 c1 = td->GetColor1();
        Vector3 c2 = td->GetColor2();
        auto* dst1 = (t == 0) ? setup.teamAColor1 : setup.teamBColor1;
        auto* dst2 = (t == 0) ? setup.teamAColor2 : setup.teamBColor2;
        dst1[0] = static_cast<uint8_t>(clamp(c1.coords[0] * 255.0f, 0.0f, 255.0f));
        dst1[1] = static_cast<uint8_t>(clamp(c1.coords[1] * 255.0f, 0.0f, 255.0f));
        dst1[2] = static_cast<uint8_t>(clamp(c1.coords[2] * 255.0f, 0.0f, 255.0f));
        dst2[0] = static_cast<uint8_t>(clamp(c2.coords[0] * 255.0f, 0.0f, 255.0f));
        dst2[1] = static_cast<uint8_t>(clamp(c2.coords[1] * 255.0f, 0.0f, 255.0f));
        dst2[2] = static_cast<uint8_t>(clamp(c2.coords[2] * 255.0f, 0.0f, 255.0f));
    }

    for (int t = 0; t < 2; ++t) {
        Team* team = match->GetTeam(t);
        if (!team) continue;
        const std::vector<Player*>& players = team->GetAllPlayers();
        for (size_t i = 0; i < players.size() && i < 11; ++i) {
            Player* p = players[i];
            if (!p) continue;
            int idx = static_cast<int>(i + t * 11);
            if (idx >= kMaxPlayers) break;

            PlayerStaticInfo& ps = setup.players[idx];
            ps.index = static_cast<uint8_t>(idx);
            ps.team = static_cast<uint8_t>(t);
            ps.role = static_cast<uint8_t>(p->GetFormationEntry().role);
            ps.playerNumber = (playerNumbers_[idx] != 0) ? playerNumbers_[idx] : static_cast<uint8_t>(i + 1); // config or fallback
            ps.bodyType = bodyTypes_[idx];
            ps.beardStyle = beardStyles_[idx];
            ps.eyeColor = eyeColors_[idx];

            const PlayerData* pd = p->GetPlayerData();
            if (pd) {
                copyString(ps.lastName, sizeof(ps.lastName), pd->GetLastName());
                ps.height = pd->GetHeight();
                ps.skinColor = static_cast<uint8_t>(pd->GetSkinColor());
                // Map string hair style/color to compact uint8 indices for network
                {
                    std::string hs = pd->GetHairStyle();
                    if (hs == "long")      ps.hairStyle = 1;
                    else if (hs == "mohawk") ps.hairStyle = 2;
                    else if (hs == "curly")  ps.hairStyle = 3;
                    else if (hs == "ponytail") ps.hairStyle = 4;
                    else if (hs == "bald")   ps.hairStyle = 5;
                    else                     ps.hairStyle = 0; // short (default)
                }
                {
                    std::string hc = pd->GetHairColor();
                    if (hc == "dark_brown")  ps.hairColor = 1;
                    else if (hc == "brown")   ps.hairColor = 2;
                    else if (hc == "light_brown") ps.hairColor = 3;
                    else if (hc == "blonde")  ps.hairColor = 4;
                    else if (hc == "red")     ps.hairColor = 5;
                    else if (hc == "grey")    ps.hairColor = 6;
                    else if (hc == "white")   ps.hairColor = 7;
                    else                      ps.hairColor = 0; // black (default)
                }
                // Extract all 22 stats (order must match skillNameToEnum)
                static const char* statNames[kNumPlayerStats] = {
                    "physical_balance", "physical_reaction", "physical_acceleration",
                    "physical_velocity", "physical_stamina", "physical_agility",
                    "physical_shotpower", "technical_standingtackle", "technical_slidingtackle",
                    "technical_ballcontrol", "technical_dribble", "technical_shortpass",
                    "technical_highpass", "technical_header", "technical_shot",
                    "technical_volley", "mental_calmness", "mental_workrate",
                    "mental_resilience", "mental_defensivepositioning", "mental_offensivepositioning",
                    "mental_vision"
                };
                for (int s = 0; s < kNumPlayerStats; ++s) {
                    ps.stats[s] = pd->GetStat(statNames[s]);
                }
            }
        }
    }

    // Prefix room_id (36 bytes) for backend routing
    std::vector<uint8_t> setupBuf(36 + sizeof(setup));
    auto roomBytes = cfg_.roomId.substr(0, 36);
    std::memcpy(setupBuf.data(), roomBytes.c_str(), roomBytes.size());
    std::memcpy(setupBuf.data() + 36, &setup, sizeof(setup));
    cfg_.redis->publishBinary("gf.setup", setupBuf.data(), setupBuf.size());
    matchSetupSent_ = true;
    std::cout << "[GameServer] MatchSetup broadcast (" << sizeof(setup) << " bytes)" << std::endl;
}

void Server::run() {
    std::cout << "[GameServer] Room " << cfg_.roomId << " starting (" << cfg_.duration << "s)" << std::endl;

    if (std::getenv("GFOOTBALL_DATA_DIR") == nullptr) {
        setenv("GFOOTBALL_DATA_DIR", "/app/data", 1);
    }
    if (std::getenv("GFOOTBALL_FONT") == nullptr) {
        setenv("GFOOTBALL_FONT", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 1);
    }
    const char* dataDir = std::getenv("GFOOTBALL_DATA_DIR");
    if (dataDir && chdir(dataDir) != 0) {
        std::cerr << "[GameServer] Warning: failed to chdir to " << dataDir << std::endl;
    }

    // Redis is already configured by main() and passed via cfg_.redis
    bool hasRedis = cfg_.redis && cfg_.redis->isConfigured();
    if (!hasRedis) {
        std::cerr << "[GameServer] WARNING: Redis not configured. No game state will be broadcast."
                  << " Set --redis-url=..." << std::endl;
    }

    gameEnv_ = new GameEnv();

    // Configure GF match parameters BEFORE start_game().
    // With the fixed start_game() (uses this->scenario_config), we must
    // pre-fill formations so GF can initialise controllers correctly.
    auto& sc = gameEnv_->scenario_config;
    sc.left_agents = (cfg_.gameMode == 2) ? 0 : 1;  // 0 for ai_vs_ai, 1 otherwise
    sc.right_agents = (cfg_.gameMode == 0) ? 1 : 0; // 1 for 1v1, 0 for vs_ai/ai_vs_ai
    sc.game_duration = cfg_.duration * dzfoot::kSimFrequencyHz;  // GF steps (100 steps/sec)

    // Load custom configuration if provided (via JSON string or path)
    MatchConfig mcfg;
    bool hasCustomConfig = false;
    if (!cfg_.matchConfigJson.empty()) {
        if (MatchConfig::loadString(cfg_.matchConfigJson, mcfg)) {
            hasCustomConfig = true;
            std::cout << "[GameServer] Loaded custom match config from JSON string" << std::endl;
        } else {
            std::cerr << "[GameServer] Failed to load match config from JSON string" << std::endl;
        }
    } else if (!cfg_.matchConfigPath.empty()) {
        if (MatchConfig::load(cfg_.matchConfigPath, mcfg)) {
            hasCustomConfig = true;
            std::cout << "[GameServer] Loaded custom match config from file: " << cfg_.matchConfigPath << std::endl;
        } else {
            std::cerr << "[GameServer] Failed to load match config from file" << std::endl;
        }
    }

    if (hasCustomConfig) {
        sc.game_duration = mcfg.duration_seconds * dzfoot::kSimFrequencyHz;
        stadiumId_ = mcfg.stadium_id;
        if (!mcfg.left_team.formation.empty()) {
            sc.left_team = buildFormation(mcfg.left_team);
        }
        if (!mcfg.right_team.formation.empty()) {
            sc.right_team = buildFormation(mcfg.right_team);
        }
        std::cout << "[GameServer] Custom formation applied ("
                  << sc.left_team.size() << " left, "
                  << sc.right_team.size() << " right players)" << std::endl;
    }

    // Pre-fill default 4-3-3 formations (avoids empty-team crash)
    bool leftControllable = (cfg_.gameMode != 2);
    bool rightControllable = (cfg_.gameMode == 0);
    if (sc.left_team.empty()) appendDefault433(sc.left_team, leftControllable, false);
    if (sc.right_team.empty()) appendDefault433(sc.right_team, rightControllable, true);

    gameEnv_->start_game();

    std::cout << "[GameServer] GF engine started (headless)" << std::endl;

    // Warm up: a few env steps so Match initialises player data fully.
    for (int i = 0; i < 5; ++i) {
        gameEnv_->step();
    }

    // Apply formations (custom or default 4-3-3) to the live match.
    // Must happen after warm-up so Match/Team objects exist.
    gameEnv_->apply_formations();
    std::cout << "[GameServer] Formations applied to live match" << std::endl;

    // Inject custom player skills from match config into GF PlayerData
    if (hasCustomConfig) {
        if (!mcfg.left_team.players.empty() || !mcfg.right_team.players.empty()) {
            std::cout << "[GameServer] Injecting custom player skills into GF engine" << std::endl;
        }
        if (gameEnv_ && gameEnv_->context && gameEnv_->context->gameTask) {
            Match* match = gameEnv_->context->gameTask->GetMatch();
            if (match) {
                auto applyTeamSkills = [&](int teamIdx, const TeamConfig& tc) {
                    if (tc.players.empty()) return;
                    Team* team = match->GetTeam(teamIdx);
                    if (!team) return;
                    const TeamData* ctd = team->GetTeamData();
                    if (!ctd) return;
                    int nPlayers = std::min((int)tc.players.size(), ctd->GetPlayerNum());
                    for (int i = 0; i < nPlayers; ++i) {
                        PlayerData* pd = ctd->GetPlayerData(i);
                        if (!pd) continue;
                        const auto& profile = tc.players[i];

                        // Inject custom avatar fields into engine PlayerData
                        pd->SetLastName(profile.name);
                        pd->SetSkinColor(profile.skinColor);
                        pd->SetHairStyle(profile.hairStyle);
                        pd->SetHairColor(profile.hairColor);
                        pd->SetHeight(profile.height);

                        for (const auto& kv : profile.skills) {
                            std::string stat = skillNameToEnum(kv.first);
                            if (!stat.empty()) {
                                pd->SetStat(stat, kv.second);
                            } else {
                                std::cerr << "[GameServer] Unknown skill '" << kv.first
                                          << "' for player " << profile.name << std::endl;
                            }
                        }
                        std::cout << "[GameServer] Skills injected for " << profile.name
                                  << " (team " << teamIdx << ", slot " << i << ")" << std::endl;
                    }
                };
                applyTeamSkills(0, mcfg.left_team);
                applyTeamSkills(1, mcfg.right_team);
                std::cout << "[GameServer] Custom player skills applied to both teams" << std::endl;
            }
        }

        // Store avatar configurations from config for MatchSetup
        for (size_t i = 0; i < mcfg.left_team.players.size() && i < 11; ++i) {
            const auto& p = mcfg.left_team.players[i];
            playerNumbers_[i] = static_cast<uint8_t>(p.number);
            bodyTypes_[i] = static_cast<uint8_t>(p.bodyType);
            beardStyles_[i] = static_cast<uint8_t>(p.beardStyle);
            eyeColors_[i] = static_cast<uint8_t>(p.eyeColor);
        }
        for (size_t i = 0; i < mcfg.right_team.players.size() && i < 11; ++i) {
            const auto& p = mcfg.right_team.players[i];
            playerNumbers_[11 + i] = static_cast<uint8_t>(p.number);
            bodyTypes_[11 + i] = static_cast<uint8_t>(p.bodyType);
            beardStyles_[11 + i] = static_cast<uint8_t>(p.beardStyle);
            eyeColors_[11 + i] = static_cast<uint8_t>(p.eyeColor);
        }
    }

    sendMatchSetup();

    if (cfg_.redis && cfg_.redis->isConfigured()) {
        cfg_.redis->publish("gf.ready", cfg_.roomId);
    }

    baseTimestampUs_ = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    // === Three independent frequencies (DO NOT MIX) ===
    // 1. Physics simulation : 100 Hz  (gameEnv_->step(), most accurate)
    // 2. State extraction     : 60 Hz  (updateState(), what goes into GameStatePacket)
    // 3. LiveKit broadcast    : 20 Hz  (broadcastGameState(), packets to Android)
    // Android receives at 20 Hz and renders at 60 Hz.
    //
    // NOTE: accumulateStats() runs inside updateState() (60 Hz), so per-tick
    // stats (possession, distance) are sampled at 60 Hz, not 100 Hz.
    // This is accurate enough for gameplay stats; do NOT move it into the
    // 100 Hz loop without also updating currentState_ there.
    const int simTicksPerSec = 100;
    const int stateUpdateHz = 60;
    const auto framePeriod = std::chrono::microseconds(1'000'000 / simTicksPerSec);
    int broadcastHz = std::min(cfg_.broadcastRateHz, simTicksPerSec);
    if (broadcastHz < 1) broadcastHz = 1;
    const int broadcastSkip = std::max(1, simTicksPerSec / broadcastHz);
    const int tacticalSkip = std::max(1, simTicksPerSec / 10);
    int stateTickAccumulator = 0;

    while (running_) {
        if (cfg_.shutdownFlag && !*(cfg_.shutdownFlag)) {
            std::cout << "[GameServer] Shutdown requested, stopping loop" << std::endl;
            running_ = false;
            break;
        }
        auto frameStart = std::chrono::steady_clock::now();

        // 1. Physics step at 100 Hz
        try {
            applyPendingInputs();
            gameEnv_->step();
            ++tickCounter_;
        } catch (const std::exception& e) {
            std::cerr << "[GameServer] Exception in step(): " << e.what() << std::endl;
            if (cfg_.redis && cfg_.redis->isConfigured()) {
                cfg_.redis->publish("gf.crashed", cfg_.roomId);
            }
            running_ = false;
            break;
        } catch (...) {
            std::cerr << "[GameServer] Unknown exception in step()" << std::endl;
            if (cfg_.redis && cfg_.redis->isConfigured()) {
                cfg_.redis->publish("gf.crashed", cfg_.roomId);
            }
            running_ = false;
            break;
        }

        // 2. State update at 60 Hz (accumulator: 60/100 ratio)
        stateTickAccumulator += stateUpdateHz;
        if (stateTickAccumulator >= simTicksPerSec) {
            stateTickAccumulator -= simTicksPerSec;
            try {
                updateState();
            } catch (const std::exception& e) {
                std::cerr << "[GameServer] Exception in updateState(): " << e.what() << std::endl;
                if (cfg_.redis && cfg_.redis->isConfigured()) {
                    cfg_.redis->publish("gf.crashed", cfg_.roomId);
                }
                running_ = false;
                break;
            } catch (...) {
                std::cerr << "[GameServer] Unknown exception in updateState()" << std::endl;
                if (cfg_.redis && cfg_.redis->isConfigured()) {
                    cfg_.redis->publish("gf.crashed", cfg_.roomId);
                }
                running_ = false;
                break;
            }
        }

        // 3. Broadcast at 20 Hz (or cfg_.broadcastRateHz)
        if (tickCounter_ % broadcastSkip == 0) {
            broadcastGameState();
        }
        if (tickCounter_ % tacticalSkip == 0) {
            updateTacticalState();
            broadcastTacticalState();
        }

        // 4. 1 Hz heartbeat (every 100 ticks)
        if (tickCounter_ % simTicksPerSec == 0 && cfg_.redis && cfg_.redis->isConfigured()) {
            auto now = std::chrono::system_clock::now().time_since_epoch();
            auto sec = std::chrono::duration_cast<std::chrono::seconds>(now).count();
            cfg_.redis->hset("gf.heartbeat", cfg_.roomId, std::to_string(sec));
        }

        auto elapsed = std::chrono::steady_clock::now() - frameStart;
        auto sleepTime = framePeriod - elapsed;
        if (sleepTime.count() > 0) {
            std::this_thread::sleep_for(sleepTime);
        }
    }

    if (cfg_.redis && cfg_.redis->isConfigured()) {
        cfg_.redis->publish("gf.finished", cfg_.roomId);
    }

    delete gameEnv_;
    gameEnv_ = nullptr;
}

void Server::updateState() {
    currentState_.tick = tickCounter_;
    if (tickCounter_ % 100 == 0) {
        std::cout << "[GameServer] tick " << tickCounter_ << " timer=" << currentState_.timer << "s" << std::endl;
    }
    // 100 env-steps/s -> each tick = 10 ms wall time
    const uint64_t usPerTick = 1'000'000ULL / static_cast<uint64_t>(dzfoot::kSimFrequencyHz);
    currentState_.timestampUs = baseTimestampUs_ + static_cast<uint64_t>(tickCounter_) * usPerTick;
    currentState_.timer = tickCounter_ * (1.0f / static_cast<float>(dzfoot::kSimFrequencyHz));

    if (gameEnv_) {
        auto info = gameEnv_->get_info();

        // --- Ball state ---
        currentState_.ball.pos[0] = info.ball_position.env_coord(0);
        currentState_.ball.pos[1] = info.ball_position.env_coord(1);
        currentState_.ball.pos[2] = info.ball_position.env_coord(2);
        currentState_.ball.ownedTeam = static_cast<int8_t>(info.ball_owned_team);
        currentState_.ball.ownedPlayer = static_cast<int8_t>(info.ball_owned_player);
        currentState_.gameMode = static_cast<uint8_t>(info.game_mode);
        currentState_.gameFlags = (info.is_in_play ? 1 : 0);

        // Try to get real ball velocity & rotation from internal Ball object
        Match* match = gameEnv_->context->gameTask->GetMatch();
        if (match) {
            Ball* ball = match->GetBall();
            if (ball) {
                Vector3 mov = ball->GetMovement();
                Vector3 rot = ball->GetRotation();
                // Velocity: internal units per physics tick -> env coords per SECOND
                // Client DeadReckoning expects m/s (or env-units/s), not per-tick.
                currentState_.ball.vel[0] = (mov.coords[0] / X_FIELD_SCALE) * dzfoot::kSimFrequencyHz;
                currentState_.ball.vel[1] = (mov.coords[1] / Y_FIELD_SCALE) * dzfoot::kSimFrequencyHz;
                currentState_.ball.vel[2] = (mov.coords[2] / Z_FIELD_SCALE) * dzfoot::kSimFrequencyHz;
                currentState_.ball.rot[0] = rot.coords[0];
                currentState_.ball.rot[1] = rot.coords[1];
                currentState_.ball.rot[2] = rot.coords[2];
            }
        }

        // --- Players state ---
        auto fillTeam = [&](int teamId, int baseIdx) {
            Match* m = gameEnv_->context->gameTask->GetMatch();
            if (!m) return;
            Team* t = m->GetTeam(teamId);
            if (!t) return;
            std::vector<Player*> players;
            t->GetAllPlayers(players);
            
            // vi3itor engine: no mirroring. Use raw positions directly.
            for (size_t i = 0; i < players.size() && i < 11; ++i) {
                int idx = baseIdx + static_cast<int>(i);
                if (idx >= kMaxPlayers) break;
                Player* p = players[i];
                if (!p) continue;
                NetworkPlayerState& ps = currentState_.players[idx];
                Vector3 pos = p->GetPosition();

                // We read the stable world environment coordinates directly from the engine's GetTeamState
                // exported in `info`. This is 100% stable, handles goalkeeper and set-piece resets perfectly,
                // and completely avoids any C++ raw memory mirroring/unmirroring glitches.
                const std::vector<PlayerInfo>& teamInfos = (teamId == 0) ? info.left_team : info.right_team;
                if (i < teamInfos.size()) {
                    const PlayerInfo& pi = teamInfos[i];
                    ps.pos[0] = pi.player_position.env_coord(0);
                    ps.pos[1] = pi.player_position.env_coord(1);
                    ps.pos[2] = pi.player_position.env_coord(2);
                } else {
                    ps.pos[0] = pos.coords[0] / X_FIELD_SCALE;
                    ps.pos[1] = pos.coords[1] / Y_FIELD_SCALE;
                    ps.pos[2] = pos.coords[2] / Z_FIELD_SCALE;
                }

                ps.team = static_cast<uint8_t>(teamId);
                ps.role = static_cast<uint8_t>(p->GetFormationEntry().role);
                ps.tiredFactor = 1.0f - p->GetFatigueFactorInv();
                ps.flags = 0;
                if (p->IsActive()) ps.flags |= 1;
                if (p->HasCards()) ps.flags |= 2;
                if (p == t->MainSelectedPlayer()) ps.flags |= 4;
                if (info.ball_owned_team == teamId && info.ball_owned_player == static_cast<int>(i)) {
                    ps.flags |= 8; // has_possession
                }

                Vector3 d = p->GetDirectionVec();
                ps.dir[0] = d.coords[0];
                ps.dir[1] = d.coords[1];
                ps.dir[2] = d.coords[2];
                if (std::abs(ps.dir[0]) < 0.001f && std::abs(ps.dir[1]) < 0.001f) {
                    Vector3 mov = p->GetMovement();
                    ps.dir[0] = mov.coords[0] / X_FIELD_SCALE;
                    ps.dir[1] = mov.coords[1] / Y_FIELD_SCALE;
                    ps.dir[2] = mov.coords[2] / Z_FIELD_SCALE;
                }
                ps.rotY = getHeadingFromDir(ps.dir[0], ps.dir[1]);

                // Compute velocity by differentiating position (env coords / tick)
                if (previousState_.tick == 0) {
                    ps.vel[0] = 0.0f;
                    ps.vel[1] = 0.0f;
                    ps.vel[2] = 0.0f;
                } else {
                    NetworkPlayerState& prev = previousState_.players[idx];
                    ps.vel[0] = ps.pos[0] - prev.pos[0];
                    ps.vel[1] = ps.pos[1] - prev.pos[1];
                    ps.vel[2] = ps.pos[2] - prev.pos[2];
                }

                // Deduce animation using internal GF state when available
                ps.anim = deduceAnimId(static_cast<int>(i), teamId);
                // Override with celebration if goal was just scored by this player
                int globalIdx = baseIdx + static_cast<int>(i);
                if (globalIdx < kMaxPlayers && tickCounter_ < celebrationExpiryTick_[globalIdx]) {
                    ps.anim = ANIM_CELEBRATE;
                }
            }
        };

        fillTeam(0, 0);
        fillTeam(1, 11);

        // --- Officials (referee + 2 linesmen) ---
        if (match) {
            Officials* officials = match->GetOfficials();
            if (officials) {
                struct OffInfo { PlayerOfficial* p; uint8_t role; };
                OffInfo offs[3] = {
                    { officials->GetReferee(), 0 },
                    { officials->GetLinesmanNorth(), 1 },
                    { officials->GetLinesmanSouth(), 2 }
                };
                for (int o = 0; o < 3; ++o) {
                    PlayerOfficial* off = offs[o].p;
                    NetworkOfficialState& os = currentState_.officials[o];
                    if (off) {
                        Vector3 pos = off->GetPosition();
                        os.pos[0] = pos.coords[0] / X_FIELD_SCALE;
                        os.pos[1] = pos.coords[1] / Y_FIELD_SCALE;
                        os.pos[2] = pos.coords[2] / Z_FIELD_SCALE;
                        Vector3 d = off->GetDirectionVec();
                        os.dir[0] = d.coords[0];
                        os.dir[1] = d.coords[1];
                        os.dir[2] = d.coords[2];
                        os.rotY = getHeadingFromDir(os.dir[0], os.dir[1]);
                        float vel = off->GetFloatVelocity();
                        if (vel < 0.5f) os.anim = dzfoot::ANIM_IDLE;
                        else if (vel < 3.0f) os.anim = dzfoot::ANIM_WALK;
                        else if (vel < 6.0f) os.anim = dzfoot::ANIM_RUN;
                        else os.anim = dzfoot::ANIM_SPRINT;
                        os.team = 2; // officials team ID
                        os.role = offs[o].role;
                        os.flags = 1; // active
                    } else {
                        std::memset(&os, 0, sizeof(os));
                    }
                }
            }
        }

        // Log every 100 ticks (1 second at 100Hz) for debugging
        if (tickCounter_ % 100 == 0) {
            std::cout << "[GameServer::tick] tick=" << tickCounter_
                      << " mode=" << static_cast<int>(info.game_mode)
                      << " in_play=" << (info.is_in_play ? 1 : 0)
                      << " p0.pos=" << currentState_.players[0].pos[0] << "," << currentState_.players[0].pos[1]
                      << " p11.pos=" << currentState_.players[11].pos[0] << "," << currentState_.players[11].pos[1]
                      << " ball=" << currentState_.ball.pos[0] << "," << currentState_.ball.pos[1]
                      << std::endl;
        }

        // --- Detect card transitions (yellow = has_card flag turned on, red = is_active flag turned off) ---
        for (int idx = 0; idx < kMaxPlayers; ++idx) {
            uint8_t prevF = previousState_.players[idx].flags;
            uint8_t curF  = currentState_.players[idx].flags;
            bool prevActive = (prevF & 1) != 0;
            bool curActive  = (curF  & 1) != 0;
            bool prevCard   = (prevF & 2) != 0;
            bool curCard    = (curF  & 2) != 0;
            uint8_t team = currentState_.players[idx].team;

            if (!prevCard && curCard) {
                MatchEvent ev{};
                ev.eventType = EVENT_YELLOW_CARD;
                ev.team = team;
                ev.playerIdx = static_cast<uint8_t>(idx);
                ev.pos[0] = currentState_.players[idx].pos[0];
                ev.pos[1] = currentState_.players[idx].pos[1];
                ev.pos[2] = currentState_.players[idx].pos[2];
                ev.tick = tickCounter_;
                ev.score[0] = static_cast<uint8_t>(info.left_goals);
                ev.score[1] = static_cast<uint8_t>(info.right_goals);
                eventQueue_.push_back(ev);
                stats_.yellow_cards[team]++;
            }
            // Red card = player was active and now inactive (and not at init)
            if (prevActive && !curActive && previousState_.tick > 0) {
                MatchEvent ev{};
                ev.eventType = EVENT_RED_CARD;
                ev.team = team;
                ev.playerIdx = static_cast<uint8_t>(idx);
                ev.pos[0] = currentState_.players[idx].pos[0];
                ev.pos[1] = currentState_.players[idx].pos[1];
                ev.pos[2] = currentState_.players[idx].pos[2];
                ev.tick = tickCounter_;
                ev.score[0] = static_cast<uint8_t>(info.left_goals);
                ev.score[1] = static_cast<uint8_t>(info.right_goals);
                eventQueue_.push_back(ev);
                stats_.red_cards[team]++;
            }
        }

        // --- Detect referee events (foul, offside) ---
        if (match) {
            Referee* referee = match->GetReferee();
            if (referee) {
                // Foul detection: transition from no foul to foul
                int foulType = referee->GetCurrentFoulType();
                Player* foulPlayer = referee->GetCurrentFoulPlayer();
                int foulPlayerIdx = -1;
                int foulTeam = -1;
                if (foulPlayer) {
                    // Find player index in current state
                    for (int i = 0; i < kMaxPlayers; ++i) {
                        if (currentState_.players[i].flags == 0) continue;
                        // Heuristic: match by position proximity (engine may not expose player ID directly)
                        // Fallback: use last touch team/player when referee signals foul
                        foulTeam = currentState_.ball.ownedTeam;
                        if (foulTeam >= 0 && foulTeam <= 1) {
                            int base = foulTeam * 11;
                            for (int j = 0; j < 11; ++j) {
                                if (currentState_.players[base + j].flags & 1) {
                                    foulPlayerIdx = j;
                                    break;
                                }
                            }
                        }
                        break;
                    }
                }
                if (foulType > 0 && lastRefereeFoulType_ == 0) {
                    MatchEvent ev{};
                    ev.eventType = EVENT_FOUL;
                    ev.team = (foulTeam >= 0) ? static_cast<uint8_t>(foulTeam) : 0;
                    ev.playerIdx = (foulPlayerIdx >= 0) ? static_cast<uint8_t>(foulPlayerIdx) : 0;
                    ev.pos[0] = currentState_.ball.pos[0];
                    ev.pos[1] = currentState_.ball.pos[1];
                    ev.pos[2] = currentState_.ball.pos[2];
                    ev.tick = tickCounter_;
                    ev.score[0] = static_cast<uint8_t>(info.left_goals);
                    ev.score[1] = static_cast<uint8_t>(info.right_goals);
                    eventQueue_.push_back(ev);
                }
                lastRefereeFoulType_ = foulType;

                // Offside detection: transition from no offside to offside
                bool isOffside = referee->HasOffsidePlayers();
                if (isOffside && !wasOffside_ && (tickCounter_ - lastOffsideTick_ > 100)) {
                    Player* offPlayer = referee->GetFirstOffsidePlayer();
                    if (offPlayer) {
                        // Find player index in our state arrays
                        int offTeam = -1;
                        int offIdx = 0;
                        for (int t = 0; t < 2 && offTeam < 0; ++t) {
                            Team* team = match->GetTeam(t);
                            if (!team) continue;
                            std::vector<Player*> players;
                            team->GetAllPlayers(players);
                            for (size_t j = 0; j < players.size() && j < 11; ++j) {
                                if (players[j] == offPlayer) {
                                    offTeam = t;
                                    offIdx = static_cast<int>(j);
                                    break;
                                }
                            }
                        }
                        if (offTeam < 0) {
                            // Fallback: use ball-owned team
                            offTeam = currentState_.ball.ownedTeam;
                            if (offTeam < 0 || offTeam > 1) offTeam = 0;
                        }
                        MatchEvent ev{};
                        ev.eventType = EVENT_OFFSIDE;
                        ev.team = static_cast<uint8_t>(offTeam);
                        ev.playerIdx = static_cast<uint8_t>(offIdx);
                        ev.pos[0] = currentState_.ball.pos[0];
                        ev.pos[1] = currentState_.ball.pos[1];
                        ev.pos[2] = currentState_.ball.pos[2];
                        ev.tick = tickCounter_;
                        ev.score[0] = static_cast<uint8_t>(info.left_goals);
                        ev.score[1] = static_cast<uint8_t>(info.right_goals);
                        eventQueue_.push_back(ev);
                        lastOffsideTick_ = tickCounter_;
                    }
                }
                wasOffside_ = isOffside;
            }
        }

        // Save current state for next velocity computation
        previousState_ = currentState_;

        currentState_.score[0] = static_cast<uint8_t>(info.left_goals);
        currentState_.score[1] = static_cast<uint8_t>(info.right_goals);

        // --- Detect events ---
        auto resolveShotsOnGoal = [&](int teamId, int scorerIdx) {
            // Only the scorer's pending shot counts as on-target
            int idx = teamId * 11 + scorerIdx;
            if (idx >= 0 && idx < kMaxPlayers && pendingShotExpiryTick_[idx] > 0) {
                stats_.shots_on_target[teamId]++;
                pendingShotExpiryTick_[idx] = 0;
            }
            // Clear any other pending shots without counting (missed)
            for (int i = teamId * 11; i < teamId * 11 + 11; ++i) {
                if (i != idx) pendingShotExpiryTick_[i] = 0;
            }
        };
        if (info.left_goals > lastScoreA_) {
            resolveShotsOnGoal(0, info.ball_owned_player);
            MatchEvent ev;
            ev.eventType = EVENT_GOAL;
            ev.team = 0;
            ev.playerIdx = static_cast<uint8_t>(info.ball_owned_player);
            ev.pos[0] = currentState_.ball.pos[0];
            ev.pos[1] = currentState_.ball.pos[1];
            ev.pos[2] = currentState_.ball.pos[2];
            ev.tick = tickCounter_;
            ev.score[0] = static_cast<uint8_t>(info.left_goals);
            ev.score[1] = static_cast<uint8_t>(info.right_goals);
            eventQueue_.push_back(ev);
            stats_.goals[0]++;
            lastScoreA_ = info.left_goals;
            // Force celebration on scorer (~3 seconds at 100Hz)
            int scorerIdx = info.ball_owned_player;
            if (scorerIdx >= 0 && scorerIdx < 11) {
                celebrationExpiryTick_[scorerIdx] = tickCounter_ + 300; // 3 sec
            }
        }
        if (info.right_goals > lastScoreB_) {
            resolveShotsOnGoal(1, info.ball_owned_player);
            MatchEvent ev;
            ev.eventType = EVENT_GOAL;
            ev.team = 1;
            ev.playerIdx = static_cast<uint8_t>(info.ball_owned_player);
            ev.pos[0] = currentState_.ball.pos[0];
            ev.pos[1] = currentState_.ball.pos[1];
            ev.pos[2] = currentState_.ball.pos[2];
            ev.tick = tickCounter_;
            ev.score[0] = static_cast<uint8_t>(info.left_goals);
            ev.score[1] = static_cast<uint8_t>(info.right_goals);
            eventQueue_.push_back(ev);
            stats_.goals[1]++;
            lastScoreB_ = info.right_goals;
            // Force celebration on scorer (~3 seconds at 100Hz)
            int scorerIdx = info.ball_owned_player;
            if (scorerIdx >= 0 && scorerIdx < 11) {
                celebrationExpiryTick_[11 + scorerIdx] = tickCounter_ + 300; // 3 sec
            }
        }

        // Game mode transitions -> events
        if (lastGameMode_ != static_cast<uint8_t>(info.game_mode)) {
            EventType et = EVENT_KICK_OFF;
            switch (info.game_mode) {
                case e_SetPiece_KickOff:   et = EVENT_KICK_OFF; break;
                case e_SetPiece_GoalKick:  et = EVENT_GOAL_KICK; break;
                case e_SetPiece_Corner:    et = EVENT_CORNER; break;
                case e_SetPiece_FreeKick:  et = EVENT_FREE_KICK; break;
                case e_SetPiece_ThrowIn:   et = EVENT_THROW_IN; break;
                case e_SetPiece_Penalty:   et = EVENT_PENALTY; break;
                default: et = EVENT_KICK_OFF; break;
            }
            if (info.game_mode != e_SetPiece_None) {
                MatchEvent ev;
                ev.eventType = et;
                ev.team = 0;
                ev.playerIdx = 0;
                ev.pos[0] = currentState_.ball.pos[0];
                ev.pos[1] = currentState_.ball.pos[1];
                ev.pos[2] = currentState_.ball.pos[2];
                ev.tick = tickCounter_;
                ev.score[0] = static_cast<uint8_t>(info.left_goals);
                ev.score[1] = static_cast<uint8_t>(info.right_goals);
                eventQueue_.push_back(ev);
            }
            lastGameMode_ = static_cast<uint8_t>(info.game_mode);
        }

        // Half time (once, when the timer crosses duration / 2)
        if (!halfTimeSent_ && currentState_.timer >= cfg_.duration * 0.5f) {
            MatchEvent ev{};
            ev.eventType = EVENT_HALF_TIME;
            ev.team = 0;
            ev.playerIdx = 0;
            ev.pos[0] = 0; ev.pos[1] = 0; ev.pos[2] = 0;
            ev.tick = tickCounter_;
            ev.score[0] = static_cast<uint8_t>(info.left_goals);
            ev.score[1] = static_cast<uint8_t>(info.right_goals);
            ev.extra = 1; // first half ended
            eventQueue_.push_back(ev);
            halfTimeSent_ = true;
        }

        // End of match
        if (currentState_.timer >= cfg_.duration) {
            MatchEvent ev;
            ev.eventType = EVENT_END_MATCH;
            ev.team = 0;
            ev.playerIdx = 0;
            ev.pos[0] = 0; ev.pos[1] = 0; ev.pos[2] = 0;
            ev.tick = tickCounter_;
            ev.score[0] = static_cast<uint8_t>(info.left_goals);
            ev.score[1] = static_cast<uint8_t>(info.right_goals);
            eventQueue_.push_back(ev);
            running_ = false;
        }
    }

    accumulateStats();
}

// ------------------------------------------------------------------
// Animation deducer using internal GF state when available
// ------------------------------------------------------------------
// Helper: decide if the ball is to the left or right of a player
static float ballSideRelative(const Player* p, const Ball* ball) {
    if (!p || !ball) return 0.0f;
    Vector3 playerPos = p->GetPosition();
    Vector3 ballPos = ball->Predict(0); // current ball position
    Vector3 forward = p->GetDirectionVec();
    Vector3 ballRel(ballPos.coords[0] - playerPos.coords[0],
                    ballPos.coords[1] - playerPos.coords[1],
                    ballPos.coords[2] - playerPos.coords[2]);
    // 2D cross product (forward.x * rel.z - forward.z * rel.x)
    return forward.coords[0] * ballRel.coords[2] - forward.coords[2] * ballRel.coords[0];
}

uint8_t Server::deduceAnimId(int playerIndex, int teamId) {
    if (!gameEnv_) return ANIM_IDLE;

    Match* match = gameEnv_->context->gameTask->GetMatch();
    if (!match) return ANIM_IDLE;

    Team* team = match->GetTeam(teamId);
    if (!team) return ANIM_IDLE;

    const std::vector<Player*>& players = team->GetAllPlayers();
    if (playerIndex < 0 || playerIndex >= static_cast<int>(players.size())) return ANIM_IDLE;
    Player* p = players[playerIndex];
    if (!p) return ANIM_IDLE;

    e_FunctionType ft = p->GetCurrentFunctionType();
    e_Velocity vel = p->GetEnumVelocity();
    bool hasBall = p->HasPossession();
    bool isGK = (currentState_.players[teamId * 11 + playerIndex].role == e_PlayerRole_GK);
    Ball* ball = match->GetBall();

    // Helper: map movement velocity to visual locomotion anim based on ball possession.
    // GF's "dribble" velocity (~3.5 m/s) and "walk" velocity (~5.0 m/s) are both RUN
    // visually when the player does NOT have the ball. Only when controlling the
    // ball do we show DRIBBLE for these moderate speeds.
    auto movementAnim = [&](e_Velocity v) -> uint8_t {
        if (hasBall) {
            // With ball: any non-idle movement is dribbling
            switch (v) {
                case e_Velocity_Idle:    return ANIM_DRIBBLE; // standing with ball at feet
                case e_Velocity_Dribble: return ANIM_DRIBBLE; // ~3.5 m/s with ball
                case e_Velocity_Walk:    return ANIM_DRIBBLE; // ~5.0 m/s with ball
                case e_Velocity_Sprint:  return ANIM_DRIBBLE; // rare but clamp to dribble
                default:                 return ANIM_DRIBBLE;
            }
        } else {
            // Without ball: interpret GF velocity as real-world locomotion
            switch (v) {
                case e_Velocity_Idle:    return ANIM_IDLE;    // 0 m/s
                case e_Velocity_Dribble: return ANIM_RUN;     // ~3.5 m/s = moderate run
                case e_Velocity_Walk:    return ANIM_RUN;     // ~5.0 m/s = normal run (NOT walk!)
                case e_Velocity_Sprint:  return ANIM_SPRINT;  // ~8.0 m/s = sprint
                default:                 return ANIM_IDLE;
            }
        }
    };

    // --- GOALKEEPER ---
    if (isGK) {
        switch (ft) {
            case e_FunctionType_Catch:     return ANIM_GK_CATCH;
            case e_FunctionType_Deflect: {
                float side = ballSideRelative(p, ball);
                return (side > 0.0f) ? ANIM_GK_DIVE_R : ANIM_GK_DIVE_L;
            }
            case e_FunctionType_Shot:
            case e_FunctionType_Sliding:
            case e_FunctionType_Trip: {
                float side = ballSideRelative(p, ball);
                return (side > 0.0f) ? ANIM_GK_DIVE_R : ANIM_GK_DIVE_L;
            }
            default: break;
        }
        // GK locomotion: idle for all non-action states
        switch (vel) {
            case e_Velocity_Idle:    return ANIM_GK_IDLE;
            case e_Velocity_Walk:    return ANIM_GK_IDLE;
            case e_Velocity_Dribble: return ANIM_GK_IDLE;
            case e_Velocity_Sprint:  return ANIM_GK_IDLE;
            default: break;
        }
        return ANIM_GK_IDLE;
    }

    // --- FIELD PLAYERS: action-type animations (override locomotion) ---
    switch (ft) {
        case e_FunctionType_Shot: {
            // Heuristic: choose foot based on shooting direction vs dominant foot
            Vector3 dir = p->GetDirectionVec();
            bool rightFoot = (playerIndex % 2 == 0);
            bool shootingRight = (teamId == 0 && dir.coords[0] > 0.0f) ||
                                 (teamId == 1 && dir.coords[0] < 0.0f);
            if (shootingRight) return rightFoot ? ANIM_SHOOT_R : ANIM_SHOOT_L;
            return rightFoot ? ANIM_SHOOT_L : ANIM_SHOOT_R;
        }
        case e_FunctionType_ShortPass:  return ANIM_PASS_S;
        case e_FunctionType_LongPass:
        case e_FunctionType_HighPass:   return ANIM_PASS_L;
        case e_FunctionType_Header:     return ANIM_HEADER;
        case e_FunctionType_Sliding:    return ANIM_TACKLE;
        case e_FunctionType_Trip:       return ANIM_FALL;
        case e_FunctionType_Deflect:
        case e_FunctionType_Interfere:  return ANIM_TACKLE;
        case e_FunctionType_Special:    return ANIM_CELEBRATE;

        // Trap/BallControl are transitional: show dribble if controlling the ball,
        // otherwise fall back to normal locomotion (running toward the ball).
        case e_FunctionType_Trap:
        case e_FunctionType_BallControl: {
            if (hasBall) return ANIM_DRIBBLE;
            return movementAnim(vel);
        }

        default: break; // e_FunctionType_Movement falls through to locomotion logic
    }

    // --- FIELD PLAYERS: default locomotion (e_FunctionType_Movement) ---
    return movementAnim(vel);
}

float Server::getHeadingFromDir(float dx, float dz) const {
    return std::atan2(dx, dz); // GF coordinate convention
}

int Server::findPlayerIndex(Team* team, Player* player) const {
    if (!team || !player) return 255;
    const std::vector<Player*>& players = team->GetAllPlayers();
    for (size_t i = 0; i < players.size() && i < 11; ++i) {
        if (players[i] == player) return static_cast<int>(i);
    }
    return 255;
}

int Server::findPlayerIndex(Team* team, int playerID) const {
    if (!team || playerID < 0) return 255;
    const std::vector<Player*>& players = team->GetAllPlayers();
    for (size_t i = 0; i < players.size() && i < 11; ++i) {
        if (players[i] && players[i]->GetID() == playerID) return static_cast<int>(i);
    }
    return 255;
}

uint8_t Server::deduceAiIntent(Player* player, TeamAIController* controller, int playerIndex, int teamId) {
    if (!player || !controller) return TACTICAL_INTENT_NONE;
    if (player->HasPossession()) return TACTICAL_INTENT_HAS_BALL;
    if (controller->GetPieceTaker() == player) return TACTICAL_INTENT_SET_PIECE_TAKER;
    if (controller->GetAttackingRunPlayer() == player) return TACTICAL_INTENT_ATTACKING_RUN;
    if (controller->GetTeamPressurePlayer() == player) return TACTICAL_INTENT_PRESS;
    if (controller->GetForwardSupportPlayer() == player) return TACTICAL_INTENT_SUPPORT;
    if (player->GetManMarkingID() >= 0) return TACTICAL_INTENT_MARK;
    if (player->GetTimeNeededToGetToBall_ms() < 500) return TACTICAL_INTENT_CHASE_BALL;
    return TACTICAL_INTENT_HOLD_FORMATION;
}

void Server::updateTacticalState() {
    std::memset(&tacticalState_, 0, sizeof(tacticalState_));
    tacticalState_.tick = tickCounter_;
    tacticalState_.timestampUs = currentState_.timestampUs;
    tacticalState_.setPieceTeam = 255;
    tacticalState_.setPieceTaker = 255;
    tacticalState_.selectedPlayer[0] = 255;
    tacticalState_.selectedPlayer[1] = 255;
    tacticalState_.designatedPlayer[0] = 255;
    tacticalState_.designatedPlayer[1] = 255;
    tacticalState_.bestPossessionPlayer[0] = 255;
    tacticalState_.bestPossessionPlayer[1] = 255;
    tacticalState_.lastTouchTeam = 255;
    tacticalState_.lastTouchPlayer = 255;
    for (int i = 0; i < DZ_MAX_PLAYERS; ++i) {
        tacticalState_.players[i].targetTeam = 255;
        tacticalState_.players[i].targetPlayer = 255;
    }
    if (!gameEnv_ || !gameEnv_->context || !gameEnv_->context->gameTask) return;

    Match* match = gameEnv_->context->gameTask->GetMatch();
    if (!match) return;

    tacticalState_.matchPhase = static_cast<uint8_t>(match->GetMatchPhase());
    tacticalState_.lastTouchTeam = static_cast<uint8_t>(match->GetLastTouchTeamID() >= 0 ? match->GetLastTouchTeamID() : 255);
    if (match->GetLastTouchTeamID() >= 0) {
        tacticalState_.lastTouchPlayer = static_cast<uint8_t>(findPlayerIndex(match->GetTeam(match->GetLastTouchTeamID()), match->GetLastTouchPlayer()));
    }

    for (int t = 0; t < 2; ++t) {
        Team* team = match->GetTeam(t);
        if (!team) continue;
        TeamAIController* controller = team->GetController();
        tacticalState_.selectedPlayer[t] = static_cast<uint8_t>(findPlayerIndex(team, team->MainSelectedPlayer()));
        tacticalState_.designatedPlayer[t] = static_cast<uint8_t>(findPlayerIndex(team, team->GetDesignatedTeamPossessionPlayer()));
        tacticalState_.bestPossessionPlayer[t] = static_cast<uint8_t>(findPlayerIndex(team, team->GetBestPossessionPlayer()));
        if (controller) {
            tacticalState_.offsideTrapX[t] = controller->GetOffsideTrapX() / X_FIELD_SCALE;
            if (controller->GetSetPieceType() != e_SetPiece_None) {
                tacticalState_.setPieceType = static_cast<uint8_t>(controller->GetSetPieceType());
                tacticalState_.setPieceTaker = static_cast<uint8_t>(findPlayerIndex(team, controller->GetPieceTaker()));
                if (tacticalState_.setPieceTaker != 255) tacticalState_.setPieceTeam = static_cast<uint8_t>(t);
            }
        }

        const std::vector<Player*>& players = team->GetAllPlayers();
        for (size_t i = 0; i < players.size() && i < 11; ++i) {
            Player* p = players[i];
            if (!p) continue;
            int idx = static_cast<int>(t * 11 + i);
            TacticalPlayerState& out = tacticalState_.players[idx];
            Vector3 formationTarget = controller ? controller->GetAdaptedFormationPosition(p, true) : p->GetPosition();
            out.formationTarget[0] = formationTarget.coords[0] / X_FIELD_SCALE;
            out.formationTarget[1] = formationTarget.coords[1] / Y_FIELD_SCALE;
            out.formationTarget[2] = formationTarget.coords[2] / Z_FIELD_SCALE;
            out.targetPos[0] = out.formationTarget[0];
            out.targetPos[1] = out.formationTarget[1];
            out.targetPos[2] = out.formationTarget[2];
            float dx = out.formationTarget[0] - currentState_.players[idx].pos[0];
            float dz = out.formationTarget[2] - currentState_.players[idx].pos[2];
            out.formationDistance = std::sqrt(dx * dx + dz * dz);
            out.stamina01 = std::max(0.0f, std::min(1.0f, 1.0f - currentState_.players[idx].tiredFactor));
            out.staticRole = static_cast<uint8_t>(p->GetFormationEntry().role);
            out.dynamicRole = static_cast<uint8_t>(p->GetDynamicFormationEntry().role);
            out.functionType = static_cast<uint8_t>(p->GetCurrentFunctionType());
            out.velocityType = static_cast<uint8_t>(p->GetEnumVelocity());
            out.aiIntent = deduceAiIntent(p, controller, static_cast<int>(i), t);
            if (p->TouchPending()) out.actionFlags |= 1;
            if (p->TouchAnim()) out.actionFlags |= 2;
            if (p->HasPossession()) out.actionFlags |= 4;
            if (p->ExternalControllerActive()) out.tacticalFlags |= 1;
            if (!p->ExternalControllerActive()) out.tacticalFlags |= 2;
            if (p->GetFormationEntry().controllable) out.tacticalFlags |= 4;
            if (team->MainSelectedPlayer() == p) out.tacticalFlags |= 8;
            if (team->GetDesignatedTeamPossessionPlayer() == p) out.tacticalFlags |= 16;
            if (team->GetBestPossessionPlayer() == p) out.tacticalFlags |= 32;
            int markID = p->GetManMarkingID();
            if (markID >= 0) {
                out.targetTeam = static_cast<uint8_t>(1 - t);
                out.targetPlayer = static_cast<uint8_t>(findPlayerIndex(match->GetTeam(1 - t), markID));
            }
        }
    }
}

void Server::broadcastTacticalState() {
    bool hasRedis = cfg_.redis && cfg_.redis->isConfigured();
    if (!hasRedis) return;

    tacticalState_.header.magic = dzfoot::DZ_MAGIC;
    tacticalState_.header.version = dzfoot::DZ_PROTOCOL_VERSION;
    tacticalState_.header.type = dzfoot::PACKET_TACTICAL_STATE;
    tacticalState_.header.size = static_cast<uint16_t>(sizeof(tacticalState_));
    tacticalState_.header.flags = 0;
    auto roomBytes = cfg_.roomId.substr(0, 36);

    std::vector<uint8_t> buf(36 + sizeof(tacticalState_));
    std::memcpy(buf.data(), roomBytes.c_str(), roomBytes.size());
    std::memcpy(buf.data() + 36, &tacticalState_, sizeof(tacticalState_));
    cfg_.redis->publishBinary("gf.tactical", buf.data(), buf.size());
}

void Server::broadcastGameState() {
    bool hasRedis = cfg_.redis && cfg_.redis->isConfigured();
    if (!hasRedis) return;

    // Fill protocol header before broadcast
    currentState_.header.magic   = dzfoot::DZ_MAGIC;
    currentState_.header.version = dzfoot::DZ_PROTOCOL_VERSION;
    currentState_.header.type    = dzfoot::PACKET_GAME_STATE;
    currentState_.header.size    = static_cast<uint16_t>(sizeof(currentState_));
    currentState_.header.flags   = 0;

    auto roomBytes = cfg_.roomId.substr(0, 36);

    // Prefix room_id (36 bytes) for backend routing
    std::vector<uint8_t> gsBuf(36 + sizeof(currentState_));
    std::memcpy(gsBuf.data(), roomBytes.c_str(), roomBytes.size());
    std::memcpy(gsBuf.data() + 36, &currentState_, sizeof(currentState_));
    cfg_.redis->publishBinary("gf.gamestate", gsBuf.data(), gsBuf.size());

    for (auto& ev : eventQueue_) {
        ev.header.magic   = dzfoot::DZ_MAGIC;
        ev.header.version = dzfoot::DZ_PROTOCOL_VERSION;
        ev.header.type    = dzfoot::PACKET_MATCH_EVENT;
        ev.header.size    = static_cast<uint16_t>(sizeof(ev));
        ev.header.flags   = 0;
        std::vector<uint8_t> evBuf(36 + sizeof(ev));
        std::memcpy(evBuf.data(), roomBytes.c_str(), roomBytes.size());
        std::memcpy(evBuf.data() + 36, &ev, sizeof(ev));
        cfg_.redis->publishBinary("gf.event", evBuf.data(), evBuf.size());
    }
    eventQueue_.clear();
}

void Server::receiveInput(const PlayerInputPacket& input) {
    std::lock_guard<std::mutex> lock(inputMutex_);
    if (inputQueue_.size() >= 64) {
        inputQueue_.pop(); // drop oldest to prevent memory flood
    }
    inputQueue_.push(input);
}

void Server::processInputs() {
    // Applied in run() loop via applyPendingInputs()
}

void Server::applyPendingInputs() {
    if (!gameEnv_) return;

    // Reset all controller states before applying new inputs.
    // This prevents "sticky" inputs when a client stops sending packets.
    gameEnv_->reset_inputs();

    std::lock_guard<std::mutex> lock(inputMutex_);
    while (!inputQueue_.empty()) {
        PlayerInput inp = inputQueue_.front();
        inputQueue_.pop();

        // Anti-cheat: validate team assignment
        if (inp.team > 1) {
            std::cerr << "[GameServer] Anti-cheat: rejected input with invalid team=" << (int)inp.team << std::endl;
            continue;
        }
        if (cfg_.gameMode == 2) {
            // ai_vs_ai: no human inputs allowed
            continue;
        }
        if (cfg_.gameMode == 1 && inp.team == 1) {
            std::cerr << "[GameServer] Anti-cheat: rejected input for AI team (mode=vs_AI)" << std::endl;
            continue;
        }
        bool left_team = (inp.team == 0);
        int player = static_cast<int>(inp.playerIdx);
        if (player < 0 || player > 10) {
            std::cerr << "[GameServer] Anti-cheat: rejected input with invalid playerIdx=" << player << std::endl;
            continue;
        }

        float dx = inp.dirX;
        float dz = inp.dirZ;
        if (!std::isfinite(dx) || !std::isfinite(dz)) {
            std::cerr << "[GameServer] Anti-cheat: rejected input with non-finite direction" << std::endl;
            continue;
        }
        // Clamp and normalize handled by sanitizePlayerInput on client, re-validate here
        if (dx < -1.0f) dx = -1.0f; if (dx > 1.0f) dx = 1.0f;
        if (dz < -1.0f) dz = -1.0f; if (dz > 1.0f) dz = 1.0f;
        float lenSq = dx*dx + dz*dz;
        if (lenSq > 1.0f) { float inv = 1.0f/std::sqrt(lenSq); dx *= inv; dz *= inv; }

        gameEnv_->action(game_release_direction, left_team, player);

        if (std::abs(dx) < 0.3f && std::abs(dz) < 0.3f) {
            gameEnv_->action(game_idle, left_team, player);
        } else {
            float angle = std::atan2(-dz, dx);
            const float sector = 3.14159265f / 8.0f;
            if (angle < -7*sector || angle >= 7*sector)       gameEnv_->action(game_right, left_team, player);
            else if (angle >= -7*sector && angle < -5*sector) gameEnv_->action(game_bottom_right, left_team, player);
            else if (angle >= -5*sector && angle < -3*sector) gameEnv_->action(game_bottom, left_team, player);
            else if (angle >= -3*sector && angle < -sector)   gameEnv_->action(game_bottom_left, left_team, player);
            else if (angle >= -sector && angle < sector)      gameEnv_->action(game_left, left_team, player);
            else if (angle >= sector && angle < 3*sector)     gameEnv_->action(game_top_left, left_team, player);
            else if (angle >= 3*sector && angle < 5*sector)   gameEnv_->action(game_top, left_team, player);
            else                                               gameEnv_->action(game_top_right, left_team, player);
        }

        if (inp.buttons & dzfoot::BUTTON_PASS)       gameEnv_->action(game_short_pass, left_team, player);
        else                                           gameEnv_->action(game_release_short_pass, left_team, player);

        if (inp.buttons & dzfoot::BUTTON_HIGH_PASS)   gameEnv_->action(game_high_pass, left_team, player);
        else                                            gameEnv_->action(game_release_high_pass, left_team, player);

        if (inp.buttons & dzfoot::BUTTON_SHOT)       gameEnv_->action(game_shot, left_team, player);
        else                                          gameEnv_->action(game_release_shot, left_team, player);

        if (inp.buttons & dzfoot::BUTTON_SLIDING)    gameEnv_->action(game_sliding, left_team, player);
        else                                          gameEnv_->action(game_release_sliding, left_team, player);

        if (inp.buttons & dzfoot::BUTTON_DRIBBLE)    gameEnv_->action(game_dribbling, left_team, player);
        else                                          gameEnv_->action(game_release_dribbling, left_team, player);

        if (inp.buttons & dzfoot::BUTTON_SPRINT)     gameEnv_->action(game_sprint, left_team, player);
        else                                          gameEnv_->action(game_release_sprint, left_team, player);

        if (inp.buttons & dzfoot::BUTTON_SWITCH_PLAYER) gameEnv_->action(game_switch, left_team, player);
        else                                             gameEnv_->action(game_release_switch, left_team, player);

        if (inp.buttons & dzfoot::BUTTON_KICK)       gameEnv_->action(game_long_pass, left_team, player);
        else                                          gameEnv_->action(game_release_long_pass, left_team, player);
    }
}

void Server::accumulateStats() {
    // Possession based on ball proximity (per tick)
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

    // Distance: sum |velocity| per player per tick. Velocity is in env-coords/tick
    // so multiplying by field length gives metres travelled this tick.
    // GF pitch is roughly 105m x 68m; env coords are normalised to [-1, 1] on X, [-0.42, 0.42] on Y.
    constexpr float kPitchLenM = 105.0f * 0.5f; // env_coord 1.0 = half pitch
    constexpr float kPitchWidM = 68.0f * 0.5f;
    for (int idx = 0; idx < kMaxPlayers; ++idx) {
        const auto& ps = currentState_.players[idx];
        float vx = ps.vel[0] * kPitchLenM;
        float vz = ps.vel[2] * kPitchWidM;
        float dist = std::sqrt(vx * vx + vz * vz);
        stats_.distance_m[ps.team] += dist;
    }

    // Shots / passes / tackles via GF function-type transitions.
    if (!gameEnv_) return;
    Match* match = gameEnv_->context->gameTask->GetMatch();
    if (!match) return;

    int8_t ownedTeam = currentState_.ball.ownedTeam;
    int8_t ownedPlayer = currentState_.ball.ownedPlayer;

    for (int t = 0; t < 2; ++t) {
        Team* team = match->GetTeam(t);
        if (!team) continue;
        const std::vector<Player*>& players = team->GetAllPlayers();
        for (size_t i = 0; i < players.size() && i < 11; ++i) {
            Player* p = players[i];
            if (!p) continue;
            int idx = static_cast<int>(t * 11 + i);
            uint8_t prevFt = prevFunctionType_[idx];
            uint8_t curFt = static_cast<uint8_t>(p->GetCurrentFunctionType());

            if (curFt != prevFt) {
                // Start-of-action transitions (count once per occurrence).
                switch (curFt) {
                    case e_FunctionType_Shot:
                        stats_.shots[t]++;
                        // Mark a pending shot: resolved by goal or keeper save within ~6s.
                        pendingShotExpiryTick_[idx] = static_cast<int>(tickCounter_) + 100;
                        break;
                    case e_FunctionType_ShortPass:
                    case e_FunctionType_LongPass:
                    case e_FunctionType_HighPass:
                        stats_.passes[t]++;
                        // Pending pass resolved if same team owns ball within ~0.5s, different player.
                        pendingPassExpiryTick_[idx] = static_cast<int>(tickCounter_) + 50;
                        pendingPassPlayerIdx_[idx] = static_cast<int>(i);
                        break;
                    case e_FunctionType_Sliding:
                        stats_.tackles[t]++;
                        break;
                    default: break;
                }
            }
            prevFunctionType_[idx] = curFt;

            // Resolve pending pass: same team holds the ball, different player.
            if (pendingPassExpiryTick_[idx] > 0) {
                if (ownedTeam == t &&
                    ownedPlayer >= 0 &&
                    ownedPlayer != pendingPassPlayerIdx_[idx]) {
                    stats_.passes_success[t]++;
                    pendingPassExpiryTick_[idx] = 0;
                } else if (static_cast<int>(tickCounter_) >= pendingPassExpiryTick_[idx]) {
                    pendingPassExpiryTick_[idx] = 0;
                }
            }

            // Resolve pending shot: opposing keeper Catch/Deflect counts as on-target.
            // Goals scored after a shot are also counted as on-target via the goal-event
            // detection in updateState() which calls noteShotOnTarget() (see below).
            if (pendingShotExpiryTick_[idx] > 0) {
                bool onTarget = false;
                Team* opp = match->GetTeam(1 - t);
                if (opp) {
                    const std::vector<Player*>& oppP = opp->GetAllPlayers();
                    if (!oppP.empty() && oppP[0]) {
                        e_FunctionType gkFt = oppP[0]->GetCurrentFunctionType();
                        if (gkFt == e_FunctionType_Catch || gkFt == e_FunctionType_Deflect) {
                            onTarget = true;
                        }
                    }
                }
                if (onTarget) {
                    stats_.shots_on_target[t]++;
                    pendingShotExpiryTick_[idx] = 0;
                } else if (static_cast<int>(tickCounter_) >= pendingShotExpiryTick_[idx]) {
                    pendingShotExpiryTick_[idx] = 0;
                }
            }
        }
    }
}

void Server::detectEvents() {
    // Events are handled directly in updateState()
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

    std::cout << "[GameServer] Match finished. Score: "
              << static_cast<int>(stats_.score[0]) << " - " << static_cast<int>(stats_.score[1])
              << " | Possession: " << stats_.possession_ticks[0] << "% - "
              << stats_.possession_ticks[1] << "%"
              << std::endl;

    StatsPoster::post(statsUrl, cfg_, stats_);
}

// ------------------------------------------------------------------
// Map JSON skill name string → GF stat name (empty = unknown)
// ------------------------------------------------------------------
std::string skillNameToEnum(const std::string& name) {
    if (name == "physical_balance")            return "physical_balance";
    if (name == "physical_reaction")           return "physical_reaction";
    if (name == "physical_acceleration")       return "physical_acceleration";
    if (name == "physical_velocity")           return "physical_velocity";
    if (name == "physical_stamina")            return "physical_stamina";
    if (name == "physical_agility")            return "physical_agility";
    if (name == "physical_shotpower")          return "physical_shotpower";
    if (name == "technical_standingtackle")    return "technical_standingtackle";
    if (name == "technical_slidingtackle")     return "technical_slidingtackle";
    if (name == "technical_ballcontrol")       return "technical_ballcontrol";
    if (name == "technical_dribble")           return "technical_dribble";
    if (name == "technical_shortpass")         return "technical_shortpass";
    if (name == "technical_highpass")          return "technical_highpass";
    if (name == "technical_header")            return "technical_header";
    if (name == "technical_shot")              return "technical_shot";
    if (name == "technical_volley")            return "technical_volley";
    if (name == "mental_calmness")             return "mental_calmness";
    if (name == "mental_workrate")             return "mental_workrate";
    if (name == "mental_resilience")           return "mental_resilience";
    if (name == "mental_defensivepositioning") return "mental_defensivepositioning";
    if (name == "mental_offensivepositioning") return "mental_offensivepositioning";
    if (name == "mental_vision")               return "mental_vision";
    return ""; // sentinel / unknown
}

} // namespace GameServer
