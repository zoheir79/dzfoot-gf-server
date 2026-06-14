// DZFoot Headless Environment — wraps vi3itor engine for 100Hz server simulation
// Inspired by Google Brain's headless design (MockRenderer3D), but without RL/mirroring.

#ifndef _DZFOOT_ENV_HPP
#define _DZFOOT_ENV_HPP

#include "../gamedefines.hpp"

#include <SDL2/SDL_ttf.h>

#include <vector>
#include <string>

namespace blunted { class Properties; }

class IHIDevice;
class Match;
class GameTask;
class Team;
class Player;
class Ball;

// Simplified state struct for DZFoot server extraction
struct DZFootMatchState {
  float ballPos[3];
  float ballVel[3];
  int matchPhase;  // 0=1stHalf, 1=2ndHalf, etc.
  bool inPlay;
  bool goalScored;
  int score[2];
  unsigned long matchTime_ms;
  int game_mode;        // e_SetPiece equivalent
  int ball_owned_team;
  int ball_owned_player;
};

struct DZFootPlayerState {
  int teamID;
  int playerID;
  float pos[3];
  float vel[3];
  float orientation[4];  // quaternion
  bool isActive;
};

// DZFoot environment class — minimal interface for headless server
class DZFootEnv {
 public:
  DZFootEnv();
  ~DZFootEnv();

  // Initialize the engine headlessly (no display, no audio, no menus)
  void Initialize(int resX = 1280, int resY = 720);

  // Start a match with given team database IDs
  void StartMatch(int team1DbID, int team2DbID);

  // Advance one simulation tick (100 Hz = one call per 10 ms)
  void Step();

  // Set controller input for a player
  // team: 0 or 1, player: 0-10
  void SetDirection(int team, int player, float dx, float dy);
  void SetButton(int team, int player, int buttonFunc, bool state);

  // Reset all inputs (call after each tick)
  void ResetInputs();

  // Extract current match state
  DZFootMatchState GetMatchState();
  std::vector<DZFootPlayerState> GetPlayerStates();

  // Direct access to match / game task (for advanced state extraction)
  Match* GetMatch();
  GameTask* GetGameTask();

  // Check if match is running
  bool IsMatchRunning() const;

  // Shutdown
  void Shutdown();

 private:
  void DoInitialize(blunted::Properties *config);
  void CreateControllers();

  bool initialized_ = false;
  bool matchRunning_ = false;
  int step_ = 0;

  // Controllers: 2 teams x 11 players = 22 remote controllers
  std::vector<IHIDevice*> controllers_;

  // Font handles (closed in Shutdown after MenuTask is destroyed)
  TTF_Font* defaultFont_ = nullptr;
  TTF_Font* defaultOutlineFont_ = nullptr;
};

#endif
