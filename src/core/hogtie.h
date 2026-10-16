#pragma once
// [Gestures] ForceHogtie (2026-10-16, the user: "hogtie doesn't work", the game's "press Y to hogtie" did nothing
// though its Y, LT and RT all arrived): on foot, someone on the player's lasso (GET_LASSO_TARGET) within ForceHogtieRange,
// a press of the game's Y hogties them with HOGTIE_ACTOR unless the game has hogtied them itself 0.4 s later.

namespace rdrvr::hogtie {

void init();             // the config and the worker thread
void on_game_y();        // the Y the game gets was just pressed (controls.cpp, the pad's rising edge). Any thread.
bool enabled();
void set_enabled(bool on);  // also written to the user ini

}  // namespace rdrvr::hogtie
