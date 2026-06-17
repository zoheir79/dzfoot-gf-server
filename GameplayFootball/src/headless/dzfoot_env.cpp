// DZFoot Headless Environment — wraps vi3itor engine for 100Hz server simulation

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
#include "../utils/database.hpp"

// Global variables defined in main.cpp, used for headless init/shutdown
extern Properties *config;
extern GraphicsSystem *graphicsSystem;
extern Database *db;
extern boost::shared_ptr<Scene2D> scene2D;
extern boost::shared_ptr<Scene3D> scene3D;
extern boost::shared_ptr<GameTask> gameTask;
extern boost::shared_ptr<MenuTask> menuTask;
extern std::vector<IHIDevice*> controllers;

#include "../onthepitch/match.hpp"
#include "../onthepitch/team.hpp"
#include "../onthepitch/player/player.hpp"
#include "../onthepitch/ball.hpp"

#include <SDL2/SDL_ttf.h>
#include <ctime>
#include <cstring>
#include <chrono>
#include <iostream>
#include <cstdlib>

using namespace blunted;

DZFootEnv::DZFootEnv() {}

DZFootEnv::~DZFootEnv() {
  if (initialized_) Shutdown();
}

void DZFootEnv::Initialize(int resX, int resY) {
  if (initialized_) return;

  // Config properties — assign to the global config variable defined in main.cpp
  config = new Properties();
  config->SetBool("render", false);  // headless
  config->SetInt("context_x", resX);
  config->SetInt("context_y", resY);
  config->SetInt("context_bpp", 32);
  config->SetInt("context_fullscreen", 0);
  config->Set("graphics3d_renderer", "opengl");
  config->SetInt("physics_frametime_ms", 10);  // 10 ms = 100 Hz physics

  DoInitialize(config);

  // Font loading (required by MenuTask even if not rendering)
  std::string fontfilename;
  const char* fontEnv = std::getenv("GFOOTBALL_FONT");
  if (fontEnv) {
    fontfilename = fontEnv;
  } else {
    fontfilename = config->Get("font_filename", "media/fonts/alegreya/AlegreyaSansSC-ExtraBold.ttf");
    const char* dataDir = std::getenv("GFOOTBALL_DATA_DIR");
    if (dataDir && !fontfilename.empty() && fontfilename[0] != '/') {
      fontfilename = std::string(dataDir) + "/" + fontfilename;
    }
  }
  defaultFont_ = TTF_OpenFont(fontfilename.c_str(), 32);
  if (!defaultFont_) {
    Log(e_FatalError, "dzfoot", "Initialize", "Could not load font " + fontfilename);
  }
  defaultOutlineFont_ = TTF_OpenFont(fontfilename.c_str(), 32);
  TTF_SetFontOutline(defaultOutlineFont_, 2);

  // Initialize systems
  graphicsSystem = new GraphicsSystem();
  SystemManager::GetInstancePtr()->RegisterSystem("GraphicsSystem", graphicsSystem);
  graphicsSystem->Initialize(false, resX, resY);  // headless: MockRenderer3D, no gfx thread

  // Init scenes
  scene2D = boost::shared_ptr<Scene2D>(new Scene2D("scene2D", *config));
  SceneManager::GetInstance().RegisterScene(scene2D);
  // NOTE: do NOT delete config here — IsReleaseVersion() and other code
  // access the global config pointer later. Small one-time leak is acceptable
  // for a headless server lifetime.

  scene3D = boost::shared_ptr<Scene3D>(new Scene3D("scene3D"));
  SceneManager::GetInstance().RegisterScene(scene3D);

  // Create game task
  gameTask = boost::shared_ptr<GameTask>(new GameTask());

  // Create menu task (needed for MatchData)
  menuTask = boost::shared_ptr<MenuTask>(new MenuTask(5.0f / 4.0f, 0, defaultFont_, defaultOutlineFont_));

  // Create remote controllers
  CreateControllers();

  initialized_ = true;
}

void DZFootEnv::DoInitialize(Properties *config) {
  // Initialize the Blunted2 engine core (managers, systems, scheduler, scene)
  blunted::Initialize(*config);

  srand((unsigned int)time(NULL));
  rand();  // mingw32 first value bug workaround
  randomseed();
  fastrandomseed();

  // Database (needed for team/player data)
  db = new Database();
  std::string dbPath = "databases/default/database.sqlite";
  const char* dataDir = std::getenv("GFOOTBALL_DATA_DIR");
  if (dataDir) {
    dbPath = std::string(dataDir) + "/" + dbPath;
  }
  bool dbSuccess = db->Load(dbPath);
  if (!dbSuccess) {
    Log(e_FatalError, "dzfoot", "DoInitialize", "Could not open database: " + dbPath);
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

void DZFootEnv::StartMatch(int team1DbID, int team2DbID, int leftAgents, int rightAgents) {
  if (!initialized_) {
    Log(e_FatalError, "dzfoot", "StartMatch", "Environment not initialized");
    return;
  }

  // Create match data
  MatchData *matchData = new MatchData(team1DbID, team2DbID);
  menuTask->SetMatchData(matchData);

  // Set controller setup: only register human gamers for configured agents.
  // In vs_AI mode (leftAgents=1, rightAgents=0) only 1 human gamer is created
  // for team 0; the remaining 10 field players on team 0 and all 11 on team 1
  // are controlled by the AI (ElizaController).
  std::vector<SideSelection> setup;
  for (int i = 0; i < std::min(leftAgents, playerNum); i++) {
    SideSelection s; s.controllerID = i; s.side = -1; setup.push_back(s);
  }
  for (int i = 0; i < std::min(rightAgents, playerNum); i++) {
    SideSelection s; s.controllerID = i + playerNum; s.side = 1; setup.push_back(s);
  }
  menuTask->SetControllerSetup(setup);

  // Start the match
  gameTask->Action(e_GameTaskMessage_StartMatch);
  matchRunning_ = true;
  step_ = 0;
}

void DZFootEnv::Step() {
  if (!matchRunning_) {
    return;
  }

  // === GameSequence (sequential headless tick) ===
  // NOTE: menuTask phases are intentionally skipped — the MenuTask
  // is only needed for MatchData storage and controller setup in
  // headless mode.  Its ProcessPhase() contains a menu state machine
  // that would call gameTask->Action(StopMatch) on the first tick.

  gameTask->GetPhase();       // inputs / match->Get()
  gameTask->ProcessPhase();   // physics / match->Process()

  // === GraphicsSequence (continues sequentially in headless) ===
  gameTask->PutPhase();       // match->Put() + fullbody model update + upload geometry

  // GraphicsTask — executed sequentially (no separate thread in headless)
  ISystemTask* gfxTask = graphicsSystem->GetTask();
  gfxTask->GetPhase();        // collect visibles, poke cameras
  gfxTask->ProcessPhase();    // shadow maps, render camera, overlay2D
  gfxTask->PutPhase();        // SwapBuffers → MockRenderer3D no-op

  step_++;

  // Save previous button states AFTER all controller processing is done.
  // This must happen after HumanController has read GetPreviousButtonState()
  // so that on the NEXT tick prevButtons reflects the state from THIS tick.
  for (unsigned int i = 0; i < controllers_.size(); i++) {
    HIDRemoteController* rc = static_cast<HIDRemoteController*>(controllers_.at(i));
    rc->SavePrevState();
  }
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

  // Set piece / ball ownership
  state.game_mode = e_SetPiece_None;
  Team *team0 = match->GetTeam(0);
  if (team0 && team0->GetController()) {
    state.game_mode = (int)team0->GetController()->GetSetPieceType();
  }
  state.ball_owned_team = match->GetLastTouchTeamID();
  Player *lastTouch = match->GetLastTouchPlayer();
  state.ball_owned_player = lastTouch ? lastTouch->GetID() : -1;

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

GameTask* DZFootEnv::GetGameTask() {
  if (!gameTask) return nullptr;
  return gameTask.get();
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

  if (defaultFont_) { TTF_CloseFont(defaultFont_); defaultFont_ = nullptr; }
  if (defaultOutlineFont_) { TTF_CloseFont(defaultOutlineFont_); defaultOutlineFont_ = nullptr; }

  for (unsigned int i = 0; i < controllers.size(); i++) {
    delete controllers.at(i);
  }
  controllers.clear();
  controllers_.clear();

  scene2D.reset();
  scene3D.reset();

  // NOTE: do NOT delete graphicsSystem here — SystemManager::Exit()
  // already iterates registered systems, calls Exit() and deletes them.
  delete db;
  db = nullptr;

  Exit();  // blunted engine shutdown (deletes graphicsSystem via SystemManager)

  initialized_ = false;
}
