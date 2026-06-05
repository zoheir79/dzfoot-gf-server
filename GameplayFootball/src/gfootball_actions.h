// DZFoot Headless — Action enum (adapted from Google Brain fork)

#ifndef _GFOOTBALL_ACTIONS_H
#define _GFOOTBALL_ACTIONS_H

enum Action {
  game_idle = 0,
  game_left = 1,
  game_top_left = 2,
  game_top = 3,
  game_top_right = 4,
  game_right = 5,
  game_bottom_right = 6,
  game_bottom = 7,
  game_bottom_left = 8,
  game_long_pass = 9,
  game_high_pass = 10,
  game_short_pass = 11,
  game_shot = 12,
  game_sprint = 13,
  game_release_direction = 14,
  game_release_sprint = 15,
  game_sliding = 16,
  game_dribbling = 17,
  game_release_dribbling = 18,
  game_release_long_pass = 19,
  game_release_short_pass = 20,
  game_release_high_pass = 21,
  game_release_shot = 22,
  game_release_sliding = 23,
  game_switch = 24,
  game_release_switch = 25,
  Action_Max = 26
};

static const int game_action_max = Action_Max;

#endif
