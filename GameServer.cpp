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
    std::memset(lastInput_, 0, sizeof(lastInput_));
    std::memset(hasLastInput_, 0, sizeof(hasLastInput_));
    std::memset(lastInputTick_, 0, sizeof(lastInputTick_));
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
    setup.playerCount = 22; // 11 per team
#if 0  // Disabled to reduce noise
    printf("[gamestates] GF_SETUP teamA=%s teamB=%s duration=%u players=%u\n",
           setup.teamAName, setup.teamBName, setup.durationMinutes, setup.playerCount);
    fflush(stdout);
#endif
    {
        // Simple string hash (uint8_t) — 256 possible values. Collisions are
        // extremely unlikely for the small set of stadium names DZFoot uses.
        uint8_t hash = 0;
        for (char c : stadiumId_) hash = hash * 31 + static_cast<uint8_t>(c);
        setup.stadiumId = hash;
    }

    Match* match = gameEnv_->context->gameTask->GetMatch();
    if (!match) {
        std::cerr << "[gamestates] GF_SETUP_ERR: match not available" << std::endl;
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
}

void Server::run() {
    if (std::getenv("GFOOTBALL_DATA_DIR") == nullptr) {
        setenv("GFOOTBALL_DATA_DIR", "/app/data", 1);
    }
    if (std::getenv("GFOOTBALL_FONT") == nullptr) {
        setenv("GFOOTBALL_FONT", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 1);
    }
    const char* dataDir = std::getenv("GFOOTBALL_DATA_DIR");
    if (dataDir && chdir(dataDir) != 0) {
        std::cerr << "[gamestates] GF_WARN: failed to chdir to " << dataDir << std::endl;
    }

    // Redis is already configured by main() and passed via cfg_.redis
    bool hasRedis = cfg_.redis && cfg_.redis->isConfigured();
    if (!hasRedis) {
        std::cerr << "[gamestates] GF_WARN: Redis not configured. No game state will be broadcast."
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
        } else {
            std::cerr << "[gamestates] GF_CONFIG_ERR: Failed to load match config from JSON string" << std::endl;
        }
    } else if (!cfg_.matchConfigPath.empty()) {
        if (MatchConfig::load(cfg_.matchConfigPath, mcfg)) {
            hasCustomConfig = true;
        } else {
            std::cerr << "[gamestates] GF_CONFIG_ERR: Failed to load match config from file" << std::endl;
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
    }

    // Pre-fill default 4-3-3 formations (avoids empty-team crash)
    bool leftControllable = (cfg_.gameMode != 2);
    bool rightControllable = (cfg_.gameMode == 0);
    if (sc.left_team.empty()) appendDefault433(sc.left_team, leftControllable, false);
    if (sc.right_team.empty()) appendDefault433(sc.right_team, rightControllable, true);

    gameEnv_->start_game();

    // Warm up: a few env steps so Match initialises player data fully.
    for (int i = 0; i < 5; ++i) {
        gameEnv_->step();
    }

    // Apply formations (custom or default 4-3-3) to the live match.
    // Must happen after warm-up so Match/Team objects exist.
    gameEnv_->apply_formations();

    // Inject custom player skills from match config into GF PlayerData
    if (hasCustomConfig) {
        if (!mcfg.left_team.players.empty() || !mcfg.right_team.players.empty()) {
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
                                std::cerr << "[gamestates] GF_CONFIG_WARN: Unknown skill '" << kv.first
                                          << "' for player " << profile.name << std::endl;
                            }
                        }
                    }
                };
                applyTeamSkills(0, mcfg.left_team);
                applyTeamSkills(1, mcfg.right_team);
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

#if 0  // Disabled to reduce noise; use [setpiece_track] logs for set-piece state
    {
        Match* match = (gameEnv_->context && gameEnv_->context->gameTask) ? gameEnv_->context->gameTask->GetMatch() : nullptr;
        int gameMode = match ? match->GetMatchPhase() : -1;
        std::cout << "[gamestates] GF_START room=" << cfg_.roomId
                  << " gameMode=" << cfg_.gameMode
                  << " leftAgents=" << sc.left_agents
                  << " rightAgents=" << sc.right_agents
                  << " durationSteps=" << sc.game_duration
                  << " matchPtr=" << (match ? "yes" : "NO")
                  << std::endl;
        if (match) {
            Team* t0 = match->GetTeam(0);
            Team* t1 = match->GetTeam(1);
            std::cout << "[gamestates] GF_TEAMS T0=" << (t0 ? "ok" : "NULL")
                      << " T1=" << (t1 ? "ok" : "NULL")
                      << " inPlay=" << (match->IsInPlay() ? "YES" : "no")
                      << " setPiece=" << (match->IsInSetPiece() ? 1 : 0)
                      << std::endl;
            if (t0) {
                Player* sp = t0->MainSelectedPlayer();
                std::cout << "[gamestates] GF_ACTIVE T0 selectedRole=" << (sp ? sp->GetFormationEntry().role : -1)
                          << " active=" << (sp ? sp->IsActive() : false)
                          << std::endl;
            }
        }
        std::cout << "[gamestates] GF_STARTED" << std::endl;
    }
#endif

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
            std::cerr << "[gamestates] GF_CRASH: Exception in step(): " << e.what() << std::endl;
            if (cfg_.redis && cfg_.redis->isConfigured()) {
                cfg_.redis->publish("gf.crashed", cfg_.roomId);
            }
            running_ = false;
            break;
        } catch (...) {
            std::cerr << "[gamestates] GF_CRASH: Unknown exception in step()" << std::endl;
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
                std::cerr << "[gamestates] GF_CRASH: Exception in updateState(): " << e.what() << std::endl;
                if (cfg_.redis && cfg_.redis->isConfigured()) {
                    cfg_.redis->publish("gf.crashed", cfg_.roomId);
                }
                running_ = false;
                break;
            } catch (...) {
                std::cerr << "[gamestates] GF_CRASH: Unknown exception in updateState()" << std::endl;
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
    // 100 env-steps/s -> each tick = 10 ms wall time
    const uint64_t usPerTick = 1'000'000ULL / static_cast<uint64_t>(dzfoot::kSimFrequencyHz);
    currentState_.timestampUs = baseTimestampUs_ + static_cast<uint64_t>(tickCounter_) * usPerTick;
    currentState_.timer = tickCounter_ * (1.0f / static_cast<float>(dzfoot::kSimFrequencyHz));

    if (gameEnv_) {
        auto info = gameEnv_->get_info();

        // Use match internal time for the timer. matchTime_ms only advances
        // during active play (not during set pieces), so the Android timer
        // won't count up while players are frozen in a set piece.
        currentState_.timer = static_cast<float>(info.match_time_ms) / 1000.0f;

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

            // --- Camera state ---
            Vector3 camPos;
            Quaternion camRot;
            float camFov;
            match->GetCameraState(camPos, camRot, camFov);

            currentState_.camera.pos[0] = camPos.coords[0] / X_FIELD_SCALE;
            currentState_.camera.pos[1] = camPos.coords[1] / Y_FIELD_SCALE;
            currentState_.camera.pos[2] = camPos.coords[2] / Z_FIELD_SCALE;

            currentState_.camera.rot[0] = camRot.elements[0];
            currentState_.camera.rot[1] = camRot.elements[1];
            currentState_.camera.rot[2] = camRot.elements[2];
            currentState_.camera.rot[3] = camRot.elements[3];

            currentState_.camera.fov = camFov;
        } else {
            std::memset(&currentState_.camera, 0, sizeof(currentState_.camera));
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
                if (info.ball_owned_team == teamId && info.ball_owned_player == p->GetID()) {
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

#if 0  // Disabled to reduce noise; use [setpiece_track] logs for set-piece state
        // Server-side position debug: log every 100 ticks to verify engine movement
        if (tickCounter_ % 100 == 0) {
            Match* m = match;
            int setPiece = m ? (m->IsInSetPiece() ? 1 : 0) : -1;
            int activeIdx = -1;
            for (int i = 0; i < 22; ++i) {
                if (currentState_.players[i].flags & 4) { activeIdx = i; break; }
            }
            printf("[gamestates] GF_STATE tick=%u ball=(%.3f,%.3f,%.3f) vel=(%.3f,%.3f,%.3f) cam=(%.3f,%.3f,%.3f) mode=%d inPlay=%d setPiece=%d active=%d\n",
                   tickCounter_,
                   currentState_.ball.pos[0], currentState_.ball.pos[1], currentState_.ball.pos[2],
                   currentState_.ball.vel[0], currentState_.ball.vel[1], currentState_.ball.vel[2],
                   currentState_.camera.pos[0], currentState_.camera.pos[1], currentState_.camera.pos[2],
                   currentState_.gameMode, currentState_.gameFlags, setPiece, activeIdx);
            char pbuf[4096];
            int off = 0;
            off += snprintf(pbuf + off, sizeof(pbuf) - off, "[gamestates] GF_PLAYERS ");
            for (int i = 0; i < 22 && off < (int)sizeof(pbuf) - 60; ++i) {
                off += snprintf(pbuf + off, sizeof(pbuf) - off, "p%d=(%.2f,%.2f,a=%u,rY=%.1f,f=%u) ",
                                i, currentState_.players[i].pos[0], currentState_.players[i].pos[1],
                                currentState_.players[i].anim, currentState_.players[i].rotY,
                                currentState_.players[i].flags);
            }
            printf("%s\n", pbuf);
            fflush(stdout);
        }
#endif

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

#if 0  // Disabled to reduce noise
    static int bcLogThrottle = 0;
    if ((bcLogThrottle++ % 20) == 0) {
        printf("[gamestates] GF_BROADCAST tick=%u gs_size=%zu events=%zu room=%s\n",
               currentState_.tick, gsBuf.size(), eventQueue_.size(), cfg_.roomId.c_str());
        fflush(stdout);
    }
#endif

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
    bool hasInput = (input.buttons != 0) || (std::abs(input.dirX) > 0.01f) || (std::abs(input.dirZ) > 0.01f);
    if (hasInput) {
        // Always log real inputs so we can diagnose button taps
        printf("[gamestates] GF_IN team=%u player=%u dir=(%.3f,%.3f) buttons=0x%04X magic=0x%08X ver=%u\n",
               input.team, input.playerIdx, input.dirX, input.dirZ, input.buttons,
               input.header.magic, input.header.version);
        fflush(stdout);
    } else {
        // Still log idle/keepalive inputs occasionally so we can verify 0x0000 reaches GF
        static int recvLogThrottle = 0;
        if ((recvLogThrottle++ % 60) == 0) {
            printf("[gamestates] GF_IN_IDLE team=%u player=%u dir=(%.3f,%.3f) buttons=0x%04X\n",
                   input.team, input.playerIdx, input.dirX, input.dirZ, input.buttons);
            fflush(stdout);
        }
    }
    // Store latest packet atomically; no queue, no buffering.
    newInput_ = input;
    hasNewInput_.store(true, std::memory_order_release);
    if (input.team <= 1) {
        accumulatedButtons_[input.team].fetch_or(input.buttons, std::memory_order_relaxed);
    }
}

void Server::applyPendingInputs() {
    if (!gameEnv_) return;

    // Consume the single latest input packet directly (no queue, no buffering).
    // Also grab any buttons that arrived in rapid-fire packets between ticks.
    PlayerInputPacket newest[2] = {};
    bool hasNewest[2] = {};
    uint16_t accButtons[2] = {
        accumulatedButtons_[0].exchange(0, std::memory_order_relaxed),
        accumulatedButtons_[1].exchange(0, std::memory_order_relaxed)
    };

    if (hasNewInput_.exchange(false, std::memory_order_acquire)) {
        int t = newInput_.team;
        if (t <= 1) {
            if (std::isfinite(newInput_.dirX) && std::isfinite(newInput_.dirZ)) {
                newest[t] = newInput_;
                hasNewest[t] = true;
            }
        }
    }

    for (int t = 0; t < 2; ++t) {
        // Force-assign the piece taker to the human gamer. Team::Process()
        // reassigns the human to designatedTeamPossessionPlayer (players.at(0))
        // which is NOT the taker. Without this, the human would control the
        // wrong player and never be able to trigger the set piece.
        gameEnv_->assignPieceTakerToHuman(t);

        // Update persistent state if a new packet arrived, otherwise reuse lastInput_.
        if (hasNewest[t]) {
            lastInput_[t][0] = newest[t];
            hasLastInput_[t][0] = true;
            lastInputTick_[t] = static_cast<int>(tickCounter_);
        }
        // Auto-release after 100 ticks (~1 second) of no new input.
        // With 100 Hz client this should never trigger during normal play.
        if (hasLastInput_[t][0] && (tickCounter_ - lastInputTick_[t]) > 100) {
            hasLastInput_[t][0] = false;
        }
        if (!hasLastInput_[t][0]) {
            // Explicitly release all buttons when input times out.
            // This prevents stuck inputs if the client disconnects.
            // Release ALL e_ButtonFunction values including defensive
            // counterparts to avoid stuck defensive actions.
            bool left_team = (t == 0);
            int player = 0;
            gameEnv_->set_direction(left_team, player, 0.0f, 0.0f);
            gameEnv_->set_button(left_team, player, e_ButtonFunction_ShortPass, false);
            gameEnv_->set_button(left_team, player, e_ButtonFunction_Pressure, false);
            gameEnv_->set_button(left_team, player, e_ButtonFunction_HighPass, false);
            gameEnv_->set_button(left_team, player, e_ButtonFunction_Shot, false);
            gameEnv_->set_button(left_team, player, e_ButtonFunction_TeamPressure, false);
            gameEnv_->set_button(left_team, player, e_ButtonFunction_Sliding, false);
            gameEnv_->set_button(left_team, player, e_ButtonFunction_Dribble, false);
            gameEnv_->set_button(left_team, player, e_ButtonFunction_Sprint, false);
            gameEnv_->set_button(left_team, player, e_ButtonFunction_Switch, false);
            gameEnv_->set_button(left_team, player, e_ButtonFunction_LongPass, false);
            gameEnv_->set_button(left_team, player, e_ButtonFunction_KeeperRush, false);
            continue;
        }

            const PlayerInputPacket& inp = lastInput_[t][0];
            bool left_team = (t == 0);
            // Route input to the HUMAN GAMER's controller slot, NOT the active
            // player index. DZFootEnv binds one human gamer per team to controller
            // index 0 (controllerID 0 for team 0, playerNum for team 1) via
            // SetButton/SetDirection's idx = player + team*playerNum mapping.
            // The engine itself auto-selects which player that gamer controls
            // (closest to ball / set-piece taker), so we always target slot 0.
            // Routing to the active player index (e.g. 5) writes input into an
            // unused AI controller slot, and the input is silently discarded.
            int player = 0;
            // Active player index (for logging/telemetry only).
            int activeP = -1;
            for (int i = 0; i < 11; ++i) {
                if (currentState_.players[t * 11 + i].flags & 0x04) { activeP = i; break; }
            }

            // --- DIRECTION: continuous analog, matching desktop HID GetDirection() ---
            // Desktop HIDKeyboard::GetDirection() returns Vector3 where:
            //   coords[0] (X) = length axis, Right arrow = +1 (toward opponent goal)
            //   coords[1] (Y) = width axis,  Up arrow    = +1 (toward left touchline)
            // HIDRemoteController::GetDirection() returns the Vector3 we set here.
            // HumanController::_GetHidInput() reads it directly and applies its
            // own deadzone.  So we pass the raw float direction without quantizing.
            //
            // Coordinate mapping (verified end-to-end):
            //   GF engine:  coords[0] = X = length, coords[1] = Y = width
            //   Android 3D: X = length, Z = width (from jni_main.cpp)
            //   After applyCameraRotation: dirX = along Android X = GF X = length
            //                              dirZ = along Android Z = GF Y = width
            // So: engine coords[0] = dirX (length), engine coords[1] = dirZ (width)
            float engineX = inp.dirX;
            float engineY = inp.dirZ;
            if (engineX < -1.0f) engineX = -1.0f; if (engineX > 1.0f) engineX = 1.0f;
            if (engineY < -1.0f) engineY = -1.0f; if (engineY > 1.0f) engineY = 1.0f;
            float lenSq = engineX*engineX + engineY*engineY;
            if (lenSq > 1.0f) { float inv = 1.0f/std::sqrt(lenSq); engineX *= inv; engineY *= inv; }

            // Merge accumulated rapid-tap buttons (applied once, not persisted).
            uint16_t effectiveButtons = inp.buttons | accButtons[t];

            // Unthrottled logging during set pieces to trace button state
            bool inSetPiece = gameEnv_ && gameEnv_->is_in_set_piece();
            if (inSetPiece) {
                printf("[gamestates] GF_SP t=%d hasNew=%d inp.btns=0x%04X acc.btns=0x%04X eff.btns=0x%04X tick=%d lastTick=%d\n",
                       t, (int)hasNewest[t], inp.buttons, accButtons[t], effectiveButtons,
                       (int)tickCounter_, lastInputTick_[t]);
                fflush(stdout);
            }
#if 0  // Disabled GF_ACTION to reduce noise; keep GF_SP for set-piece buttons
            bool logThis = (actionLogThrottle_++ % 200) == 0;
            if (logThis) {
                printf("[gamestates] GF_ACTION t=%d ctrl_slot=%d active_p=%d dir=(%.3f,%.3f) buttons=0x%04X ",
                       t, player, activeP, engineX, engineY, effectiveButtons);
            }

            // Set direction as continuous float (desktop-style analog input).
            // When in deadzone, set (0,0) — HumanController::_GetHidInput()
            // will use the player's current direction at idle velocity.
            gameEnv_->set_direction(left_team, player, engineX, engineY);
            if (logThis) {
                if (std::abs(engineX) < 0.05f && std::abs(engineY) < 0.05f)
                    printf("dir=idle ");
                else
                    printf("dir=(%.3f,%.3f) ", engineX, engineY);
            }

            // --- BUTTONS: dual-function mapping matching desktop defaultKeyIDs ---
            // Desktop keyboard maps the SAME key to both offensive and defensive
            // e_ButtonFunction values.  HumanController::Process() checks context
            // (possessionContext) to decide which action to trigger.  If we only
            // set the offensive function, defensive actions are silently ignored.
            //
            // Desktop defaultKeyIDs mapping (gamedefines.hpp):
            //   SDLK_s → e_ButtonFunction_ShortPass (offense) + e_ButtonFunction_Pressure (defense)
            //   SDLK_d → e_ButtonFunction_Shot (offense)      + e_ButtonFunction_TeamPressure (defense)
            //   SDLK_w → e_ButtonFunction_LongPass (offense)  + e_ButtonFunction_KeeperRush (defense)
            //   SDLK_a → e_ButtonFunction_HighPass (offense)  + e_ButtonFunction_Sliding (defense)
            //
            // On Android, TACKLE (BUTTON_SLIDING) is a separate UI button, so
            // BUTTON_HIGH_PASS only sets HighPass (not Sliding).  But BUTTON_PASS
            // must set both ShortPass + Pressure because there's no separate
            // Pressure button on the touch UI.

            // BUTTON_PASS → ShortPass (offense) + Pressure (defense)
            if (effectiveButtons & dzfoot::BUTTON_PASS) {
                gameEnv_->set_button(left_team, player, e_ButtonFunction_ShortPass, true);
                gameEnv_->set_button(left_team, player, e_ButtonFunction_Pressure, true);
                if (logThis) printf("btn=PASS ");
            } else {
                gameEnv_->set_button(left_team, player, e_ButtonFunction_ShortPass, false);
                gameEnv_->set_button(left_team, player, e_ButtonFunction_Pressure, false);
            }

            // BUTTON_HIGH_PASS → HighPass (offense only; Sliding has its own button)
            if (effectiveButtons & dzfoot::BUTTON_HIGH_PASS) {
                gameEnv_->set_button(left_team, player, e_ButtonFunction_HighPass, true);
                if (logThis) printf("btn=HIGH ");
            } else {
                gameEnv_->set_button(left_team, player, e_ButtonFunction_HighPass, false);
            }

            // BUTTON_SHOT → Shot (offense) + TeamPressure (defense)
            if (effectiveButtons & dzfoot::BUTTON_SHOT) {
                gameEnv_->set_button(left_team, player, e_ButtonFunction_Shot, true);
                gameEnv_->set_button(left_team, player, e_ButtonFunction_TeamPressure, true);
                if (logThis) printf("btn=SHOT ");
            } else {
                gameEnv_->set_button(left_team, player, e_ButtonFunction_Shot, false);
                gameEnv_->set_button(left_team, player, e_ButtonFunction_TeamPressure, false);
            }

            // BUTTON_SLIDING → Sliding (defense only; separate from HighPass)
            if (effectiveButtons & dzfoot::BUTTON_SLIDING) {
                gameEnv_->set_button(left_team, player, e_ButtonFunction_Sliding, true);
                if (logThis) printf("btn=SLIDE ");
            } else {
                gameEnv_->set_button(left_team, player, e_ButtonFunction_Sliding, false);
            }

            // BUTTON_DRIBBLE → Dribble
            if (effectiveButtons & dzfoot::BUTTON_DRIBBLE) {
                gameEnv_->set_button(left_team, player, e_ButtonFunction_Dribble, true);
                if (logThis) printf("btn=DRIB ");
            } else {
                gameEnv_->set_button(left_team, player, e_ButtonFunction_Dribble, false);
            }

            // BUTTON_SPRINT → Sprint
            if (effectiveButtons & dzfoot::BUTTON_SPRINT) {
                gameEnv_->set_button(left_team, player, e_ButtonFunction_Sprint, true);
                if (logThis) printf("btn=SPRINT ");
            } else {
                gameEnv_->set_button(left_team, player, e_ButtonFunction_Sprint, false);
            }

            // BUTTON_SWITCH_PLAYER → Switch
            if (effectiveButtons & dzfoot::BUTTON_SWITCH_PLAYER) {
                gameEnv_->set_button(left_team, player, e_ButtonFunction_Switch, true);
                if (logThis) printf("btn=SWITCH ");
            } else {
                gameEnv_->set_button(left_team, player, e_ButtonFunction_Switch, false);
            }

            // BUTTON_KICK → LongPass (offense) + KeeperRush (defense)
            if (effectiveButtons & dzfoot::BUTTON_KICK) {
                gameEnv_->set_button(left_team, player, e_ButtonFunction_LongPass, true);
                gameEnv_->set_button(left_team, player, e_ButtonFunction_KeeperRush, true);
                if (logThis) printf("btn=KICK ");
            } else {
                gameEnv_->set_button(left_team, player, e_ButtonFunction_LongPass, false);
                gameEnv_->set_button(left_team, player, e_ButtonFunction_KeeperRush, false);
            }

            if (logThis) { printf("\n"); fflush(stdout); }
#endif
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
