// DZFoot Headless Environment — wraps vi3itor engine for 60Hz server simulation

#include "dzfoot_env.hpp"
#include "hid_remote_controller.hpp"

#include "../main.hpp"
#include "../blunted.hpp"
#include "../gametask.hpp"
#include "../menu/menutask.hpp"
#include "../data/matchdata.hpp"

#include "../base/log.hpp"
#include "../base/utils.hpp"
#include "../base/properties.hpp"

#include "../scene/scene2d/scene2d.hpp"
#include "../scene/scene3d/scene3d.hpp"

#include "../managers/systemmanager.hpp"
#include "../managers/scenemanager.hpp"
#include "../managers/resourcemanagerpool.hpp"

#include "../systems/graphics/graphics_system.hpp"
#include "../systems/audio/audio_system.hpp"

#include "../framework/scheduler.hpp"

#include "../onthepitch/match.hpp"
#include "../onthepitch/team.hpp"
#include "../onthepitch/player/player.hpp"
#include "../onthepitch/ball.hpp"

#include <SDL2/SDL_ttf.h>
#include <ctime>
#include <cstring>
#include <chrono>
#include <iostream>

using namespace blunted;

DZFootEnv::DZFootEnv() {}

DZFootEnv::~DZFootEnv() {
  if (initialized_) Shutdown();
}

void DZFootEnv::Initialize(int resX, int resY) {
  if (initialized_) return;

  DoInitialize();

  // Config properties
  Properties *config = new Properties();
  config->SetBool("render", false);  // headless
  config->Set("context_x", resX);
  config->Set("context_y", resY);
  config->Set("context_bpp", 32);
  config->Set("context_fullscreen", 0);
  config->Set("graphics3d_renderer", "opengl");
  config->Set("physics_frametime_ms", 10);  // will be overridden by 60Hz patches

  // Font loading (required by MenuTask even if not rendering)
  std::string fontfilename = config->Get("font_filename", "media/fonts/alegreya/AlegreyaSansSC-ExtraBold.ttf");
  TTF_Font *defaultFont = TTF_OpenFont(fontfilename.c_str(), 32);
  if (!defaultFont) {
    Log(e_FatalError, "dzfoot", "Initialize", "Could not load font " + fontfilename);
  }
  TTF_Font *defaultOutlineFont = TTF_OpenFont(fontfilename.c_str(), 32);
  TTF_SetFontOutline(defaultOutlineFont, 2);

  // Initialize systems
  graphicsSystem = new GraphicsSystem();
  SystemManager::GetInstancePtr()->RegisterSystem("GraphicsSystem", graphicsSystem);
  graphicsSystem->Initialize(*config);

  // Init scenes
  scene2D = boost::shared_ptr<Scene2D>(new Scene2D("scene2D", *config));
  SceneManager::GetInstance().RegisterScene(scene2D);

  scene3D = boost::shared_ptr<Scene3D>(new Scene3D("scene3D"));
  SceneManager::GetInstance().RegisterScene(scene3D);

  // Create game task
  gameTask = boost::shared_ptr<GameTask>(new GameTask());

  // Create menu task (needed for MatchData)
  menuTask = boost::shared_ptr<MenuTask>(new MenuTask(5.0f / 4.0f, 0, defaultFont, defaultOutlineFont));

  // Create remote controllers
  CreateControllers();

  initialized_ = true;
}

void DZFootEnv::DoInitialize() {
  // Initialize the Blunted2 engine core
  Properties dummyConfig;
  Initialize(dummyConfig);

  srand((unsigned int)time(NULL));
  rand();  // mingw32 first value bug workaround
  randomseed();
  fastrandomseed();

  // Database (needed for team/player data)
  db = new Database();
  bool dbSuccess = db->Load("databases/default/database.sqlite");
  if (!dbSuccess) {
    Log(e_FatalError, "dzfoot", "DoInitialize", "Could not open database");
  }
}

void DZFootEnv::CreateControllers() {
  controllers.clear();
  // Create 22 remote controllers (11 per team)
  for (int i = 0; i < 2 * playerNum; i++) {
    HIDRemoteController *rc = new HIDRemoteController();
    controllers.push_back(rc);
    controllers_.push_back(rc);
  }
}

void DZFootEnv::StartMatch(int team1DbID, int team2DbID) {
  if (!initialized_) {
    Log(e_FatalError, "dzfoot", "StartMatch", "Environment not initialized");
    return;
  }

  // Create match data
  MatchData *matchData = new MatchData(team1DbID, team2DbID);
  menuTask->SetMatchData(matchData);

  // Set controller setup: first 11 controllers = team 0, next 11 = team 1
  std::vector<SideSelection> setup;
  for (int i = 0; i < playerNum; i++) {
    SideSelection s; s.controllerID = i; s.side = -1; setup.push_back(s);
  }
  for (int i = 0; i < playerNum; i++) {
    SideSelection s; s.controllerID = i + playerNum; s.side = 1; setup.push_back(s);
  }
  menuTask->SetControllerSetup(setup);

  // Start the match
  gameTask->Action(e_GameTaskMessage_StartMatch);
  matchRunning_ = true;
  step_ = 0;
}

void DZFootEnv::Step() {
  if (!matchRunning_) return;

  // Process one game tick
  gameTask->ProcessPhase();
  step_++;
}

void DZFootEnv::SetDirection(int team, int player, float dx, float dy) {
  int idx = player + (team == 0 ? 0 : playerNum);
  if (idx >= 0 && idx < (int)controllers_.size()) {
    HIDRemoteController *rc = static_cast<HIDRemoteController*>(controllers_.at(idx));
    rc->SetDirection(Vector3(dx, dy, 0));
  }
}

void DZFootEnv::SetButton(int team, int player, int buttonFunc, bool state) {
  int idx = player + (team == 0 ? 0 : playerNum);
  if (idx >= 0 && idx < (int)controllers_.size()) {
    HIDRemoteController *rc = static_cast<HIDRemoteController*>(controllers_.at(idx));
    rc->SetButton(static_cast<e_ButtonFunction>(buttonFunc), state);
  }
}

void DZFootEnv::ResetInputs() {
  for (unsigned int i = 0; i < controllers_.size(); i++) {
    HIDRemoteController *rc = static_cast<HIDRemoteController*>(controllers_.at(i));
    rc->ResetNotSticky();
  }
}

DZFootMatchState DZFootEnv::GetMatchState() {
  DZFootMatchState state;
  memset(&state, 0, sizeof(state));

  Match *match = gameTask->GetMatch();
  if (!match) return state;

  Ball *ball = match->GetBall();
  if (ball) {
    Vector3 pos = ball->Predict(0);
    state.ballPos[0] = pos.coords[0];
    state.ballPos[1] = pos.coords[1];
    state.ballPos[2] = pos.coords[2];
    Vector3 vel = ball->GetMovement();
    state.ballVel[0] = vel.coords[0];
    state.ballVel[1] = vel.coords[1];
    state.ballVel[2] = vel.coords[2];
  }

  state.matchPhase = (int)match->GetMatchPhase();
  state.inPlay = match->IsInPlay();
  state.goalScored = match->IsGoalScored();
  state.score[0] = match->GetScore(0);
  state.score[1] = match->GetScore(1);
  state.matchTime_ms = (unsigned long)match->GetMatchTime_ms();

  return state;
}

std::vector<DZFootPlayerState> DZFootEnv::GetPlayerStates() {
  std::vector<DZFootPlayerState> states;

  Match *match = gameTask->GetMatch();
  if (!match) return states;

  for (int t = 0; t < 2; t++) {
    Team *team = match->GetTeam(t);
    if (!team) continue;
    std::vector<Player*> players;
    team->GetActivePlayers(players);
    for (unsigned int i = 0; i < players.size(); i++) {
      Player *p = players[i];
      DZFootPlayerState ps;
      ps.teamID = t;
      ps.playerID = p->GetID();
      Vector3 pos = p->GetPosition();
      ps.pos[0] = pos.coords[0];
      ps.pos[1] = pos.coords[1];
      ps.pos[2] = pos.coords[2];
      Vector3 vel = p->GetMovement();
      ps.vel[0] = vel.coords[0];
      ps.vel[1] = vel.coords[1];
      ps.vel[2] = vel.coords[2];
      Vector3 dir = p->GetDirectionVec();
      ps.orientation[0] = dir.coords[0];
      ps.orientation[1] = dir.coords[1];
      ps.orientation[2] = dir.coords[2];
      ps.orientation[3] = 0.0f;
      ps.isActive = p->IsActive();
      states.push_back(ps);
    }
  }

  return states;
}

Match* DZFootEnv::GetMatch() {
  if (!gameTask) return nullptr;
  return gameTask->GetMatch();
}

bool DZFootEnv::IsMatchRunning() const {
  return matchRunning_;
}

void DZFootEnv::Shutdown() {
  if (!initialized_) return;

  if (matchRunning_) {
    gameTask->Action(e_GameTaskMessage_StopMatch);
    matchRunning_ = false;
  }

  gameTask.reset();
  menuTask.reset();

  for (unsigned int i = 0; i < controllers.size(); i++) {
    delete controllers.at(i);
  }
  controllers.clear();
  controllers_.clear();

  scene2D.reset();
  scene3D.reset();

  delete graphicsSystem;
  graphicsSystem = nullptr;

  delete db;
  db = nullptr;

  Exit();  // blunted engine shutdown

  initialized_ = false;
}
