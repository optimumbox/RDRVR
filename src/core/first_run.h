#pragma once
// [Launcher] FirstRunNotice (2026-10-20, the user: "a launcher window that runs before the game is allowed to open ...
// only the first time a user is launching the mod"): a window on the monitor, before the game's own window, saying that
// a help post needs RDRVR.log and how to send it on Discord. Continue unlocks after 30 s and the typed sentence "I have
// fully read this and understand."; it is then never shown again (%LOCALAPPDATA%\RDRVR\FirstRunNotice.read, kept apart
// from the user ini so a release's settings reset does not show it again). Closing it quits the game.

#include <windows.h>

namespace rdrvr::first_run {

void on_process_attach();  // from DllMain: notes the game's main thread
void install();            // the bootstrap, after config::load: hooks user32's CreateWindowEx (the game's first window)
// Shows the notice now if it is still to be read and this is the game's main thread (CreateWindowEx's hook, and
// D3D12CreateDevice's in case the window was made before the hook went in). Returns once it is read.
void gate(const char* where);

}  // namespace rdrvr::first_run
