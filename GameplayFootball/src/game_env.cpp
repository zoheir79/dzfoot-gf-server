// DZFoot Headless — GameEnv wrapper implementation over DZFootEnv

#include "game_env.hpp"
#include "headless/dzfoot_env.hpp"

#include "main.hpp"
#include "gametask.hpp"
#include "menu/menutask.hpp"
#include "onthepitch/match.hpp"
#include "onthepitch/team.hpp"
#include "onthepitch/player/player.hpp"
#include "onthepitch/ball.hpp"

#include <algorithm>

GameEnv::GameEnv() {}

GameEnv::~GameEnv() {
  delete context;
  delete dzfootEnv_;
}

void GameEnv::start_game() {
  dzfootEnv_ = new DZFootEnv();
  dzfootEnv_->Initialize(1280, 720);

  // Start match with default team DB IDs (1 vs 2)
  // Only register human gamers for the configured agent counts.
  int leftAgents = std::min(std::max(scenario_config.left_agents, 0), 11);
  int rightAgents = std::min(std::max(scenario_config.right_agents, 0), 11);
  dzfootEnv_->StartMatch(1, 2, leftAgents, rightAgents);

  // Expose gameTask for GameServer.cpp direct access
  context = new GameContext();
  context->gameTask = dzfootEnv_->GetGameTask();
  context->menuTask = nullptr;  // not exposed directly

  gameStarted_ = true;
  stepCount_ = 0;
}

void GameEnv::step() {
  if (!dzfootEnv_ || !gameStarted_) return;
  dzfootEnv_->Step();
  stepCount_++;
}

SharedInfo GameEnv::get_info() {
  SharedInfo info;
  if (!dzfootEnv_) return info;

  DZFootMatchState ms = dzfootEnv_->GetMatchState();
  info.ball_position.value[0] = ms.ballPos[0];
  info.ball_position.value[1] = ms.ballPos[1];
  info.ball_position.value[2] = ms.ballPos[2];
  info.ball_velocity.value[0] = ms.ballVel[0];
  info.ball_velocity.value[1] = ms.ballVel[1];
  info.ball_velocity.value[2] = ms.ballVel[2];
  info.is_in_play = ms.inPlay;
  info.game_mode = ms.game_mode;
  info.ball_owned_team = ms.ball_owned_team;
  info.ball_owned_player = ms.ball_owned_player;
  info.left_goals = ms.score[0];
  info.right_goals = ms.score[1];

  // Fill player info
  Match* match = dzfootEnv_->GetMatch();
  if (match) {
    for (int t = 0; t < 2; t++) {
      Team* team = match->GetTeam(t);
      if (!team) continue;
      std::vector<Player*> players;
      team->GetActivePlayers(players);
      std::vector<PlayerInfo>* dst = (t == 0) ? &info.left_team : &info.right_team;
      for (Player* p : players) {
        PlayerInfo pi;
        Vector3 pos = p->GetPosition();
        pi.player_position.value[0] = pos.coords[0];
        pi.player_position.value[1] = pos.coords[1];
        pi.player_position.value[2] = pos.coords[2];
        Vector3 dir = p->GetDirectionVec();
        pi.player_direction.value[0] = dir.coords[0];
        pi.player_direction.value[1] = dir.coords[1];
        pi.player_direction.value[2] = dir.coords[2];
        pi.is_active = p->IsActive();
        pi.role = p->GetFormationEntry().role;
        dst->push_back(pi);
      }
    }
  }

  return info;
}

void GameEnv::reset_inputs() {
  if (dzfootEnv_) dzfootEnv_->ResetInputs();
}

void GameEnv::apply_formations() {
  if (!dzfootEnv_) return;
  Match* match = dzfootEnv_->GetMatch();
  if (!match) return;

  for (int t = 0; t < 2; t++) {
    Team* team = match->GetTeam(t);
    if (!team) continue;
    const std::vector<FormationEntry>& formation =
        (t == 0) ? scenario_config.left_team : scenario_config.right_team;
    if (formation.empty()) continue;

    std::vector<Player*> players;
    team->GetActivePlayers(players);
    int n = std::min((int)formation.size(), (int)players.size());
    for (int i = 0; i < n; i++) {
      team->SetFormationEntry(players[i]->GetID(), formation[i]);
    }
  }

  // Force immediate repositioning so kickoff reflects new formation entries
  Ball* ball = match->GetBall();
  Vector3 ballPos = ball ? ball->Predict(0) : Vector3(0, 0, 0);
  match->ResetSituation(ballPos);
}

void GameEnv::action(int action, bool left_team, int player) {
  if (!dzfootEnv_) return;

  int team = left_team ? 0 : 1;

  switch (action) {
    case game_release_direction:
      dzfootEnv_->SetDirection(team, player, 0.0f, 0.0f);
      break;
    case game_idle:
      dzfootEnv_->SetDirection(team, player, 0.0f, 0.0f);
      break;
    case game_left:
      dzfootEnv_->SetDirection(team, player, -1.0f, 0.0f);
      break;
    case game_top_left:
      dzfootEnv_->SetDirection(team, player, -0.707f, 0.707f);
      break;
    case game_top:
      dzfootEnv_->SetDirection(team, player, 0.0f, 1.0f);
      break;
    case game_top_right:
      dzfootEnv_->SetDirection(team, player, 0.707f, 0.707f);
      break;
    case game_right:
      dzfootEnv_->SetDirection(team, player, 1.0f, 0.0f);
      break;
    case game_bottom_right:
      dzfootEnv_->SetDirection(team, player, 0.707f, -0.707f);
      break;
    case game_bottom:
      dzfootEnv_->SetDirection(team, player, 0.0f, -1.0f);
      break;
    case game_bottom_left:
      dzfootEnv_->SetDirection(team, player, -0.707f, -0.707f);
      break;
    case game_sprint:
      dzfootEnv_->SetButton(team, player, e_ButtonFunction_Sprint, true);
      break;
    case game_release_sprint:
      dzfootEnv_->SetButton(team, player, e_ButtonFunction_Sprint, false);
      break;
    case game_shot:
      dzfootEnv_->SetButton(team, player, e_ButtonFunction_Shot, true);
      break;
    case game_short_pass:
      dzfootEnv_->SetButton(team, player, e_ButtonFunction_ShortPass, true);
      break;
    case game_high_pass:
      dzfootEnv_->SetButton(team, player, e_ButtonFunction_HighPass, true);
      break;
    case game_long_pass:
      dzfootEnv_->SetButton(team, player, e_ButtonFunction_LongPass, true);
      break;
    case game_sliding:
      dzfootEnv_->SetButton(team, player, e_ButtonFunction_Sliding, true);
      break;
    case game_dribbling:
      dzfootEnv_->SetButton(team, player, e_ButtonFunction_Dribble, true);
      break;
    case game_release_dribbling:
      dzfootEnv_->SetButton(team, player, e_ButtonFunction_Dribble, false);
      break;
    case game_release_long_pass:
      dzfootEnv_->SetButton(team, player, e_ButtonFunction_LongPass, false);
      break;
    case game_release_short_pass:
      dzfootEnv_->SetButton(team, player, e_ButtonFunction_ShortPass, false);
      break;
    case game_release_high_pass:
      dzfootEnv_->SetButton(team, player, e_ButtonFunction_HighPass, false);
      break;
    case game_release_shot:
      dzfootEnv_->SetButton(team, player, e_ButtonFunction_Shot, false);
      break;
    case game_release_sliding:
      dzfootEnv_->SetButton(team, player, e_ButtonFunction_Sliding, false);
      break;
    case game_switch:
      // DZFoot: no switch needed in headless, ignored
      break;
    case game_release_switch:
      break;
    default:
      break;
  }
}
