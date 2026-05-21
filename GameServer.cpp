#include "GameServer.h"
#include "LiveKitBridge.h"
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
#include <algorithm>

#include "game_env.hpp"
#include "gfootball_actions.h"
#include "gamedefines.hpp"

// GF internal headers for deep state extraction
#include "main.hpp"
#include "onthepitch/match.hpp"
#include "onthepitch/team.hpp"
#include "onthepitch/ball.hpp"
#include "onthepitch/player/player.hpp"
#include "onthepitch/player/playerbase.hpp"
#include "onthepitch/player/humanoid/humanoid.hpp"
#include "data/playerdata.hpp"
#include "utils.hpp"

namespace GameServer {

// Forward declaration: defined at end of this namespace
PlayerStat skillNameToEnum(const std::string& name);

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
static void appendDefault433(std::vector<FormationEntry>& dst, bool controllable) {
    // (x, y, role, lazy, controllable). y will be flipped internally by FORMATION_Y_SCALE.
    dst.emplace_back(-1.0f,  0.00f, e_PlayerRole_GK, false, controllable);
    dst.emplace_back(-0.7f,  0.75f, e_PlayerRole_LB, false, controllable);
    dst.emplace_back(-1.0f,  0.25f, e_PlayerRole_CB, false, controllable);
    dst.emplace_back(-1.0f, -0.25f, e_PlayerRole_CB, false, controllable);
    dst.emplace_back(-0.7f, -0.75f, e_PlayerRole_RB, false, controllable);
    dst.emplace_back( 0.0f,  0.50f, e_PlayerRole_CM, false, controllable);
    dst.emplace_back(-0.2f,  0.00f, e_PlayerRole_CM, false, controllable);
    dst.emplace_back( 0.0f, -0.50f, e_PlayerRole_CM, false, controllable);
    dst.emplace_back( 0.6f,  0.75f, e_PlayerRole_LM, false, controllable);
    dst.emplace_back( 1.0f,  0.00f, e_PlayerRole_CF, false, controllable);
    dst.emplace_back( 0.6f, -0.75f, e_PlayerRole_RM, false, controllable);
}

// ------------------------------------------------------------------
// Server
// ------------------------------------------------------------------
Server::Server(const Config& cfg) : cfg_(cfg) {
    currentState_.timer = 0.0f;
    currentState_.tick = 0;
    currentState_.gameMode = 0;
    currentState_.score[0] = 0;
    currentState_.score[1] = 0;
    startTime_ = std::chrono::steady_clock::now();
}

// Defined here (not in header) so unique_ptr<RedisClient> can see complete type.
Server::~Server() = default;

void Server::sendMatchSetup() {
    if (!gameEnv_ || !cfg_.livekitBridge) return;

    MatchSetupPacket setup{};
    copyString(setup.teamAName, sizeof(setup.teamAName), cfg_.teamA);
    copyString(setup.teamBName, sizeof(setup.teamBName), cfg_.teamB);
    setup.durationMinutes = static_cast<uint8_t>(cfg_.duration / 60);
    setup.stadiumId = 0;

    Match* match = gameEnv_->context->gameTask->GetMatch();
    if (!match) {
        std::cerr << "[GameServer] sendMatchSetup: match not available" << std::endl;
        return;
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
            ps.role = static_cast<uint8_t>(e_PlayerRole_GK); // default; updated below if possible

            const PlayerData* pd = p->GetPlayerData();
            if (pd) {
                copyString(ps.lastName, sizeof(ps.lastName), pd->GetLastName());
                ps.height = pd->GetHeight();
                ps.skinColor = static_cast<uint8_t>(pd->GetSkinColor());
                copyString(ps.hairStyle, sizeof(ps.hairStyle), pd->GetHairStyle());
                copyString(ps.hairColor, sizeof(ps.hairColor), pd->GetHairColor());
                // Extract all 21 stats
                for (int s = 0; s < kNumPlayerStats; ++s) {
                    ps.stats[s] = pd->GetStat(static_cast<PlayerStat>(s));
                }
            }
        }
    }

    cfg_.livekitBridge->publishData(reinterpret_cast<const uint8_t*>(&setup), sizeof(setup), "setup", true);
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

    // Connect to Redis (best-effort)
    if (!cfg_.redisUrl.empty()) {
        redis_ = std::make_unique<RedisClient>();
        if (!redis_->configure(cfg_.redisUrl)) {
            std::cerr << "[GameServer] Redis configure failed for " << cfg_.redisUrl << std::endl;
        }
    }

    gameEnv_ = new GameEnv();

    // Configure GF match parameters BEFORE start_game().
    // GF loads default 4-3-3 formations from teamdata XML when left_team/right_team are empty.
    // We ONLY override the controllable-agent counts and duration.
    auto& sc = gameEnv_->scenario_config;
    sc.left_agents = 1;                           // always 1 human on left
    sc.right_agents = (cfg_.gameMode == 0) ? 1 : 0;  // 1 human for 1v1, 0 for vs AI
    sc.game_duration = cfg_.duration * 10;          // GF steps (10 steps/sec)

    gameEnv_->start_game();

    // start_game() overwrites scenario_config with empty vectors.
    // We MUST refill them so GetState() can size left/right_controllers correctly.
    if (sc.left_team.empty()) {
        for (int i = 0; i < 11; ++i)
            sc.left_team.emplace_back(0.0f, 0.0f, e_PlayerRole_GK, false, true);
    }
    if (sc.right_team.empty()) {
        for (int i = 0; i < 11; ++i)
            sc.right_team.emplace_back(0.0f, 0.0f, e_PlayerRole_GK, false, true);
    }
    // Override agent counts after start_game() reset them to defaults
    sc.left_agents = 1;
    sc.right_agents = (cfg_.gameMode == 0) ? 1 : 0;
    sc.game_duration = cfg_.duration * 10;

    std::cout << "[GameServer] GF engine started (headless)" << std::endl;

    // If external JSON config is provided, apply custom formations/duration
    if (!cfg_.matchConfigPath.empty()) {
        MatchConfig mcfg;
        if (MatchConfig::load(cfg_.matchConfigPath, mcfg)) {
            sc.game_duration = mcfg.duration_seconds * 10;
            if (!mcfg.left_team.formation.empty()) {
                sc.left_team = buildFormation(mcfg.left_team);
            }
            if (!mcfg.right_team.formation.empty()) {
                sc.right_team = buildFormation(mcfg.right_team);
            }
            std::cout << "[GameServer] Custom config applied ("
                      << sc.left_team.size() << " left, "
                      << sc.right_team.size() << " right players)" << std::endl;
        } else {
            std::cerr << "[GameServer] Failed to load match config, using defaults" << std::endl;
        }
    }

    // Warm up: a few env steps so Match initialises player data fully.
    for (int i = 0; i < 5; ++i) {
        gameEnv_->step();
    }

    // Inject custom player skills from match config into GF PlayerData
    if (!cfg_.matchConfigPath.empty()) {
        MatchConfig mcfg;
        if (MatchConfig::load(cfg_.matchConfigPath, mcfg)) {
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
                            for (const auto& kv : profile.skills) {
                                PlayerStat stat = skillNameToEnum(kv.first);
                                if (stat != player_stat_max) {
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
        }
    }

    // Wait for LiveKit data channels to be open before broadcasting setup/state.
    // This prevents silently dropped initial packets while WebRTC negotiation completes.
    if (cfg_.livekitBridge) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (!cfg_.livekitBridge->isReadyForData() &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (!cfg_.livekitBridge->isReadyForData()) {
            std::cerr << "[GameServer] LiveKit DC not ready after 15s, continuing without bridge" << std::endl;
        } else {
            std::cout << "[GameServer] LiveKit data channels ready" << std::endl;
        }
    }

    sendMatchSetup();

    if (redis_ && redis_->isConfigured()) {
        redis_->publish("gf.ready", cfg_.roomId);
    }

    baseTimestampUs_ = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    // GameplayFootball runs at 10 environment-steps/sec by design (each step()
    // advances 10 physics frames = 100 ms simulated). We cadence the loop at
    // simTicksPerSec to match that, and broadcast at broadcastRateHz (capped to
    // simTicksPerSec since we cannot produce more states than env-steps).
    const int simTicksPerSec = 10;
    const auto framePeriod = std::chrono::microseconds(1'000'000 / simTicksPerSec);
    int broadcastHz = std::min(cfg_.broadcastRateHz, simTicksPerSec);
    if (broadcastHz < 1) broadcastHz = 1;
    const int broadcastSkip = std::max(1, simTicksPerSec / broadcastHz);

    while (running_) {
        if (cfg_.shutdownFlag && !*(cfg_.shutdownFlag)) {
            std::cout << "[GameServer] Shutdown requested, stopping loop" << std::endl;
            running_ = false;
            break;
        }
        auto frameStart = std::chrono::steady_clock::now();

        try {
            tick();
        } catch (const std::exception& e) {
            std::cerr << "[GameServer] Exception in tick(): " << e.what() << std::endl;
            if (redis_ && redis_->isConfigured()) {
                redis_->publish("gf.crashed", cfg_.roomId);
            }
            running_ = false;
            break;
        } catch (...) {
            std::cerr << "[GameServer] Unknown exception in tick()" << std::endl;
            if (redis_ && redis_->isConfigured()) {
                redis_->publish("gf.crashed", cfg_.roomId);
            }
            running_ = false;
            break;
        }

        if (tickCounter_ % broadcastSkip == 0) {
            broadcastGameState();
        }

        // 1 Hz heartbeat
        if (tickCounter_ % simTicksPerSec == 0 && redis_ && redis_->isConfigured()) {
            auto now = std::chrono::system_clock::now().time_since_epoch();
            auto sec = std::chrono::duration_cast<std::chrono::seconds>(now).count();
            redis_->hset("gf.heartbeat", cfg_.roomId, std::to_string(sec));
        }

        auto elapsed = std::chrono::steady_clock::now() - frameStart;
        auto sleepTime = framePeriod - elapsed;
        if (sleepTime.count() > 0) {
            std::this_thread::sleep_for(sleepTime);
        }
    }

    if (redis_ && redis_->isConfigured()) {
        redis_->publish("gf.finished", cfg_.roomId);
    }

    delete gameEnv_;
    gameEnv_ = nullptr;
}

void Server::tick() {
    ++tickCounter_;
    currentState_.tick = tickCounter_;
    // 10 env-steps/s -> each tick = 100 ms wall time
    currentState_.timestampUs = baseTimestampUs_ + static_cast<uint64_t>(tickCounter_) * 100'000ULL;
    currentState_.timer = tickCounter_ * 0.1f;

    if (gameEnv_) {
        applyPendingInputs();
        gameEnv_->step();

        auto info = gameEnv_->get_info();

        // --- Ball state ---
        currentState_.ball.pos[0] = info.ball_position.env_coord(0);
        currentState_.ball.pos[1] = info.ball_position.env_coord(1);
        currentState_.ball.pos[2] = info.ball_position.env_coord(2);
        currentState_.ball.ownedTeam = static_cast<int8_t>(info.ball_owned_team);
        currentState_.ball.ownedPlayer = static_cast<int8_t>(info.ball_owned_player);
        currentState_.gameMode = static_cast<uint8_t>(info.game_mode);
        currentState_.flags = (info.is_in_play ? 1 : 0);

        // Try to get real ball velocity & rotation from internal Ball object
        Match* match = gameEnv_->context->gameTask->GetMatch();
        if (match) {
            Ball* ball = match->GetBall();
            if (ball) {
                Vector3 mov = ball->GetMovement();
                Vector3 rot = ball->GetRotation();
                // Velocity: internal units per tick -> env coords per tick
                currentState_.ball.vel[0] = mov.coords[0] / X_FIELD_SCALE;
                currentState_.ball.vel[1] = mov.coords[1] / Y_FIELD_SCALE;
                currentState_.ball.vel[2] = mov.coords[2] / Z_FIELD_SCALE;
                currentState_.ball.rot[0] = rot.coords[0];
                currentState_.ball.rot[1] = rot.coords[1];
                currentState_.ball.rot[2] = rot.coords[2];
            }
        }

        // --- Players state ---
        auto fillTeam = [&](const std::vector<PlayerInfo>& src, int teamId, int baseIdx) {
            for (size_t i = 0; i < src.size() && i < 11; ++i) {
                int idx = baseIdx + static_cast<int>(i);
                if (idx >= kMaxPlayers) break;
                const PlayerInfo& pi = src[i];
                GameState::PlayerState& ps = currentState_.players[idx];
                ps.pos[0] = pi.player_position.env_coord(0);
                ps.pos[1] = pi.player_position.env_coord(1);
                ps.pos[2] = pi.player_position.env_coord(2);
                ps.team = static_cast<uint8_t>(teamId);
                ps.role = static_cast<uint8_t>(pi.role);
                ps.tiredFactor = pi.tired_factor;
                ps.flags = 0;
                if (pi.is_active) ps.flags |= 1;
                if (pi.has_card)  ps.flags |= 2;
                if (pi.designated_player) ps.flags |= 4;
                if (info.ball_owned_team == teamId && info.ball_owned_player == static_cast<int>(i)) {
                    ps.flags |= 8; // has_possession
                }
                // direction vector: prefer internal unscaled GetDirectionVec
                Match* matchDir = gameEnv_->context->gameTask->GetMatch();
                if (matchDir) {
                    Team* tDir = matchDir->GetTeam(teamId);
                    if (tDir && i < tDir->GetAllPlayers().size()) {
                        Player* pDir = tDir->GetAllPlayers()[i];
                        if (pDir) {
                            Vector3 d = pDir->GetDirectionVec();
                            ps.dir[0] = d.coords[0];
                            ps.dir[1] = d.coords[1];
                            ps.dir[2] = d.coords[2];
                        }
                    }
                }
                if (std::abs(ps.dir[0]) < 0.001f && std::abs(ps.dir[2]) < 0.001f) {
                    // fallback to scaled SharedInfo
                    ps.dir[0] = pi.player_direction.env_coord(0);
                    ps.dir[1] = pi.player_direction.env_coord(1);
                    ps.dir[2] = pi.player_direction.env_coord(2);
                }
                ps.rotY = getHeadingFromDir(ps.dir[0], ps.dir[2]);

                // Compute velocity by differentiating position (env coords / tick)
                if (previousState_.tick == 0) {
                    // First frame: previousState_ is zero-initialised, zero velocity
                    ps.vel[0] = 0.0f;
                    ps.vel[1] = 0.0f;
                    ps.vel[2] = 0.0f;
                } else {
                    GameState::PlayerState& prev = previousState_.players[idx];
                    ps.vel[0] = ps.pos[0] - prev.pos[0];
                    ps.vel[1] = ps.pos[1] - prev.pos[1];
                    ps.vel[2] = ps.pos[2] - prev.pos[2];
                }

                // Deduce animation using internal GF state when available
                ps.anim = deduceAnimId(static_cast<int>(i), teamId);
            }
        };

        fillTeam(info.left_team, 0, 0);
        fillTeam(info.right_team, 1, 11);

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
                ev.event_type = EVENT_YELLOW_CARD;
                ev.team = team;
                ev.player_idx = static_cast<uint8_t>(idx);
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
                ev.event_type = EVENT_RED_CARD;
                ev.team = team;
                ev.player_idx = static_cast<uint8_t>(idx);
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
            ev.event_type = EVENT_GOAL;
            ev.team = 0;
            ev.player_idx = static_cast<uint8_t>(info.ball_owned_player);
            ev.pos[0] = currentState_.ball.pos[0];
            ev.pos[1] = currentState_.ball.pos[1];
            ev.pos[2] = currentState_.ball.pos[2];
            ev.tick = tickCounter_;
            ev.score[0] = static_cast<uint8_t>(info.left_goals);
            ev.score[1] = static_cast<uint8_t>(info.right_goals);
            eventQueue_.push_back(ev);
            stats_.goals[0]++;
            lastScoreA_ = info.left_goals;
        }
        if (info.right_goals > lastScoreB_) {
            resolveShotsOnGoal(1, info.ball_owned_player);
            MatchEvent ev;
            ev.event_type = EVENT_GOAL;
            ev.team = 1;
            ev.player_idx = static_cast<uint8_t>(info.ball_owned_player);
            ev.pos[0] = currentState_.ball.pos[0];
            ev.pos[1] = currentState_.ball.pos[1];
            ev.pos[2] = currentState_.ball.pos[2];
            ev.tick = tickCounter_;
            ev.score[0] = static_cast<uint8_t>(info.left_goals);
            ev.score[1] = static_cast<uint8_t>(info.right_goals);
            eventQueue_.push_back(ev);
            stats_.goals[1]++;
            lastScoreB_ = info.right_goals;
        }

        // Game mode transitions -> events
        static uint8_t lastGameMode = 255;
        if (lastGameMode != static_cast<uint8_t>(info.game_mode)) {
            EventType et = EVENT_KICK_OFF;
            switch (info.game_mode) {
                case e_GameMode_KickOff:   et = EVENT_KICK_OFF; break;
                case e_GameMode_GoalKick:  et = EVENT_FREE_KICK; break; // reuse FREE_KICK slot (no GOAL_KICK in proto)
                case e_GameMode_Corner:    et = EVENT_CORNER; break;
                case e_GameMode_FreeKick:  et = EVENT_FREE_KICK; break;
                case e_GameMode_ThrowIn:   et = EVENT_THROW_IN; break;
                case e_GameMode_Penalty:   et = EVENT_PENALTY; break;
                default: et = EVENT_KICK_OFF; break;
            }
            if (info.game_mode != e_GameMode_Normal) {
                MatchEvent ev;
                ev.event_type = static_cast<uint8_t>(et);
                ev.team = 0;
                ev.player_idx = 0;
                ev.pos[0] = currentState_.ball.pos[0];
                ev.pos[1] = currentState_.ball.pos[1];
                ev.pos[2] = currentState_.ball.pos[2];
                ev.tick = tickCounter_;
                ev.score[0] = static_cast<uint8_t>(info.left_goals);
                ev.score[1] = static_cast<uint8_t>(info.right_goals);
                eventQueue_.push_back(ev);
            }
            lastGameMode = static_cast<uint8_t>(info.game_mode);
        }

        // Half time (once, when the timer crosses duration / 2)
        if (!halfTimeSent_ && currentState_.timer >= cfg_.duration * 0.5f) {
            MatchEvent ev{};
            ev.event_type = EVENT_HALF_TIME;
            ev.team = 0;
            ev.player_idx = 0;
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
            ev.event_type = EVENT_END_MATCH;
            ev.team = 0;
            ev.player_idx = 0;
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
    bool isGK = (currentState_.players[teamId * 11 + playerIndex].role == e_PlayerRole_GK);
    Ball* ball = match->GetBall();

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
        if (vel == e_Velocity_Idle) return ANIM_GK_IDLE;
        if (vel == e_Velocity_Walk || vel == e_Velocity_Sprint) {
            // keeper moving normally, not diving
            return ANIM_GK_IDLE;
        }
        // any movement -> dive toward ball
        float side = ballSideRelative(p, ball);
        return (side > 0.0f) ? ANIM_GK_DIVE_R : ANIM_GK_DIVE_L;
    }

    // --- FIELD PLAYERS by function type ---
    switch (ft) {
        case e_FunctionType_Shot: {
            // Approximate foot from direction relative to goal
            Vector3 dir = p->GetDirectionVec();
            // For team 0 (left) shooting right: positive X means right-foot-like
            // This is a heuristic; dominant foot alternates by player index
            bool rightFoot = (playerIndex % 2 == 0);
            if ((teamId == 0 && dir.coords[0] > 0.0f) ||
                (teamId == 1 && dir.coords[0] < 0.0f)) {
                return rightFoot ? ANIM_SHOOT_R : ANIM_SHOOT_L;
            }
            return rightFoot ? ANIM_SHOOT_L : ANIM_SHOOT_R;
        }
        case e_FunctionType_ShortPass:  return ANIM_PASS_S;
        case e_FunctionType_LongPass:
        case e_FunctionType_HighPass:   return ANIM_PASS_L;
        case e_FunctionType_Header:     return ANIM_HEADER;
        case e_FunctionType_Trap:
        case e_FunctionType_BallControl: {
            if (vel == e_Velocity_Idle) return ANIM_IDLE;
            if (vel == e_Velocity_Dribble) return ANIM_DRIBBLE;
            if (vel == e_Velocity_Walk) return ANIM_WALK;
            return ANIM_RUN;
        }
        case e_FunctionType_Sliding:    return ANIM_TACKLE;
        case e_FunctionType_Trip:       return ANIM_FALL;
        case e_FunctionType_Deflect:
        case e_FunctionType_Interfere: {
            float side = ballSideRelative(p, ball);
            return (side > 0.0f) ? ANIM_GK_DIVE_R : ANIM_GK_DIVE_L; // reuse keeper dives for deflections
        }
        case e_FunctionType_Special:    return ANIM_CELEBRATE;
        default: break;
    }

    // Default by velocity
    switch (vel) {
        case e_Velocity_Idle:    return ANIM_IDLE;
        case e_Velocity_Dribble: return ANIM_DRIBBLE;
        case e_Velocity_Walk:    return ANIM_WALK;
        case e_Velocity_Sprint:  return ANIM_SPRINT;
    }
    return ANIM_IDLE;
}

float Server::getHeadingFromDir(float dx, float dz) const {
    return std::atan2(dx, dz); // GF coordinate convention
}

void Server::broadcastGameState() {
    if (!cfg_.livekitBridge) return;
    cfg_.livekitBridge->publishData(reinterpret_cast<const uint8_t*>(&currentState_),
                                    sizeof(currentState_), "gs", false);

    for (const auto& ev : eventQueue_) {
        cfg_.livekitBridge->publishData(reinterpret_cast<const uint8_t*>(&ev),
                                        sizeof(ev), "ev", true);
    }
    eventQueue_.clear();
}

void Server::receiveInput(const PlayerInput& input) {
    std::lock_guard<std::mutex> lock(inputMutex_);
    inputQueue_.push(input);
}

void Server::processInputs() {
    // Applied in tick() via applyPendingInputs()
}

void Server::applyPendingInputs() {
    if (!gameEnv_) return;

    std::lock_guard<std::mutex> lock(inputMutex_);
    while (!inputQueue_.empty()) {
        PlayerInput inp = inputQueue_.front();
        inputQueue_.pop();

        // Anti-cheat: validate team assignment
        if (inp.team > 1) {
            std::cerr << "[GameServer] Anti-cheat: rejected input with invalid team=" << (int)inp.team << std::endl;
            continue;
        }
        if (cfg_.gameMode == 1 && inp.team == 1) {
            std::cerr << "[GameServer] Anti-cheat: rejected input for AI team (mode=vs_AI)" << std::endl;
            continue;
        }
        bool left_team = (inp.team == 0);
        int player = static_cast<int>(inp.playerIdx);
        if (player < 0 || player > 10) player = 0; // safety clamp

        float dx = inp.dirX;
        float dz = inp.dirZ;

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

        if (inp.pass)       gameEnv_->action(game_short_pass, left_team, player);
        else                gameEnv_->action(game_release_short_pass, left_team, player);

        if (inp.highPass)   gameEnv_->action(game_high_pass, left_team, player);
        else                gameEnv_->action(game_release_high_pass, left_team, player);

        if (inp.shot)       gameEnv_->action(game_shot, left_team, player);
        else                gameEnv_->action(game_release_shot, left_team, player);

        if (inp.sliding)    gameEnv_->action(game_sliding, left_team, player);
        else                gameEnv_->action(game_release_sliding, left_team, player);

        if (inp.dribble)    gameEnv_->action(game_dribble, left_team, player);
        else                gameEnv_->action(game_release_dribble, left_team, player);

        if (inp.sprint)     gameEnv_->action(game_sprint, left_team, player);
        else                gameEnv_->action(game_release_sprint, left_team, player);

        if (inp.switchPlayer) gameEnv_->action(game_switch, left_team, player);
        else                  gameEnv_->action(game_release_switch, left_team, player);

        if (inp.kick)       gameEnv_->action(game_long_pass, left_team, player);
        else                gameEnv_->action(game_release_long_pass, left_team, player);
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
                        pendingShotExpiryTick_[idx] = static_cast<int>(tickCounter_) + 60;
                        break;
                    case e_FunctionType_ShortPass:
                    case e_FunctionType_LongPass:
                    case e_FunctionType_HighPass:
                        stats_.passes[t]++;
                        // Pending pass resolved if same team owns ball within ~3s, different player.
                        pendingPassExpiryTick_[idx] = static_cast<int>(tickCounter_) + 30;
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
            // detection in tick() which calls noteShotOnTarget() (see below).
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
    // Events are handled directly in tick()
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
// Map JSON skill name string → GF PlayerStat enum value
// ------------------------------------------------------------------
PlayerStat skillNameToEnum(const std::string& name) {
    if (name == "physical_balance")            return physical_balance;
    if (name == "physical_reaction")           return physical_reaction;
    if (name == "physical_acceleration")       return physical_acceleration;
    if (name == "physical_velocity")           return physical_velocity;
    if (name == "physical_stamina")            return physical_stamina;
    if (name == "physical_agility")            return physical_agility;
    if (name == "physical_shotpower")          return physical_shotpower;
    if (name == "technical_standingtackle")    return technical_standingtackle;
    if (name == "technical_slidingtackle")     return technical_slidingtackle;
    if (name == "technical_ballcontrol")       return technical_ballcontrol;
    if (name == "technical_dribble")           return technical_dribble;
    if (name == "technical_shortpass")         return technical_shortpass;
    if (name == "technical_highpass")          return technical_highpass;
    if (name == "technical_header")            return technical_header;
    if (name == "technical_shot")            return technical_shot;
    if (name == "technical_volley")            return technical_volley;
    if (name == "mental_calmness")             return mental_calmness;
    if (name == "mental_workrate")             return mental_workrate;
    if (name == "mental_resilience")           return mental_resilience;
    if (name == "mental_defensivepositioning") return mental_defensivepositioning;
    if (name == "mental_offensivepositioning") return mental_offensivepositioning;
    if (name == "mental_vision")               return mental_vision;
    return player_stat_max; // sentinel / unknown
}

} // namespace GameServer
