// DZFoot Headless — GameEnv wrapper over DZFootEnv (preserves old API for GameServer.cpp)

#ifndef _GAME_ENV_HPP
#define _GAME_ENV_HPP

#include "gamedefines.hpp"
#include "gfootball_actions.h"
#include "timestep_config.hpp"

#include <vector>
#include <string>

namespace blunted {
  class Match;
  class GameTask;
  class MenuTask;
}

// Forward declarations
class DZFootEnv;

struct FormationEntry {
  float x = 0.0f;
  float y = 0.0f;
  bool controllable = false;
};

struct ScenarioConfig {
  int left_agents = 1;
  int right_agents = 0;
  int game_duration = 3000;  // steps at 60Hz = 50 sec default
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
  bool is_in_play = false;
  std::vector<PlayerInfo> left_team;
  std::vector<PlayerInfo> right_team;
};

struct GameContext {
  blunted::GameTask* gameTask = nullptr;
  blunted::MenuTask* menuTask = nullptr;
};

class GameEnv {
 public:
  GameEnv();
  ~GameEnv();

  void start_game();
  void step();
  SharedInfo get_info();
  void action(int action, bool left_team, int player);

  ScenarioConfig scenario_config;
  GameContext* context = nullptr;

 private:
  DZFootEnv* dzfootEnv_ = nullptr;
  bool gameStarted_ = false;
  int stepCount_ = 0;
};

#endif
