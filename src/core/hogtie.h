#pragma once
// [Gestures] ForceHogtie (2026-10-16, the user: "hogtie doesn't work", the game's "press Y to hogtie" did nothing
// though its Y, LT and RT all arrived): on foot, someone on the player's lasso (GET_LASSO_TARGET) within ForceHogtieRange,
// a press of the game's Y hogties them with HOGTIE_ACTOR unless the game has hogtied them itself 0.4 s later (that
// hogtie has no master: a mission waiting for the player's never moves on). The game's own hogtie works since the
// mod's free props are cleared while someone is on the lasso (hogtie.cpp's watcher), so this is only a fallback.

namespace rdrvr::hogtie {

void init();             // the config, the worker thread and the lasso's watcher
void on_game_y();        // the Y the game gets was just pressed (controls.cpp, the pad's rising edge). Any thread.
bool enabled();
void set_enabled(bool on);  // also written to the user ini
void set_watch(bool on);          // [Gestures] HogtieWatch (test channel "hogtie watch on|off")
void set_force_session(bool on);  // ForceHogtie for this session only (test channel "hogtie force on|off")

}  // namespace rdrvr::hogtie
