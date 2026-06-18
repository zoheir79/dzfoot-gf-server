// DZFoot Headless — GameEnv wrapper over DZFootEnv (preserves old API for GameServer.cpp)

#ifndef _GAME_ENV_HPP
#define _GAME_ENV_HPP

#include "gamedefines.hpp"
#include "gfootball_actions.h"
#include "timestep_config.hpp"

#include <vector>
#include <string>

// Coordinate scaling: GF internal units -> env/network units
// Normalizes X (length) in [-1.0, +1.0] range, and Y (width) in [-0.43, +0.43] range
constexpr float X_FIELD_SCALE = 55.0f;
constexpr float Y_FIELD_SCALE = 83.6f;
constexpr float Z_FIELD_SCALE = 1.0f;

// Forward declarations
class DZFootEnv;
class Match;
class GameTask;
class MenuTask;

struct ScenarioConfig {
  int left_agents = 1;
  int right_agents = 0;
  int game_duration = 6000;  // steps at 100Hz = 60 sec default
  std::vector<FormationEntry> left_team;
  std::vector<FormationEntry> right_team;
  Vector3 ball_position;
};

struct Position {
  float value[3] = {0, 0, 0};

  float env_coord(int index) const {
    switch (index) {
      case 0: return value[0] / X_FIELD_SCALE;
      case 1: return value[1] / Y_FIELD_SCALE;
      case 2: return value[2] / Z_FIELD_SCALE;
      default: return 0;
    }
  }
};

struct PlayerInfo {
  Position player_position;
  Position player_direction;
  bool has_card = false;
  bool is_active = true;
  bool designated_player = false;
  float tired_factor = 0.0f;
  e_PlayerRole role = e_PlayerRole_GK;
};

struct SharedInfo {
  Position ball_position;
  Position ball_velocity;      // DZFootMatchState.ballVel (env coords per tick)
  bool is_in_play = false;
  int game_mode = 0;           // e_SetPiece equivalent
  int ball_owned_team = -1;
  int ball_owned_player = -1;
  int left_goals = 0;
  int right_goals = 0;
  std::vector<PlayerInfo> left_team;
  std::vector<PlayerInfo> right_team;
};

struct GameContext {
  GameTask* gameTask = nullptr;
  MenuTask* menuTask = nullptr;
};

class GameEnv {
 public:
  GameEnv();
  ~GameEnv();

  void start_game();
  void step();
  SharedInfo get_info();
  void action(int action, bool left_team, int player);
  void reset_inputs();

  // Helpers for server-side set-piece management
  bool is_in_set_piece() const;

  // Force-assign the piece taker to the human gamer before applying input.
  // This must happen before HumanController::Process() reads buttons.
  void assignPieceTakerToHuman(int team);

  // Apply custom formations from scenario_config to the live match.
  // Must be called after start_game() (Match exists) and before the loop.
  void apply_formations();

  ScenarioConfig scenario_config;
  GameContext* context = nullptr;

 private:
  DZFootEnv* dzfootEnv_ = nullptr;
  bool gameStarted_ = false;
  int stepCount_ = 0;
};

#endif
