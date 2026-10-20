#pragma once
// The Dead Eye X in VR (2026-10-20). The game draws its marks' X as sprites of the "DeadeyeLocks" UI movie, each placed
// where the gameplay camera sees its mark (FUN_140394300 each tick) and shown only inside that camera's frustum. In VR
// that camera is not the head's, so the marks fell outside it while painting and no X showed. Here, while that update
// runs, each X is shown and placed at a slot of its own in a band at the top of the UI target (the game still draws
// and animates it: light, then darker once locked), and each slot is drawn into both eyes where its mark is
// ([Hands] DeadEyeMarks, on); the floating HUD quad leaves the band out while marks show.
#include <string>

namespace rdrvr::deadeye {

constexpr int kMax = 32;
struct Mark {
    float world[3];  // the mark's world point
    float slot[2];   // its X's centre in the UI target's pixels
};
void init();     // the settings
void install();  // the hooks (after anchors::verify: an installed hook changes the bytes it checks)
bool enabled();
// Presenting thread: the marks shown now (updated within the last 200 ms); the crop about each slot (UI pixels, square)
// and the X's size in the eyes (degrees across the crop).
int marks(Mark* out, int max, float* crop_px, float* deg);
// The UI band the slots use (x0 y0 x1 y1, UI pixels) while marks show; false when none
bool band(int out[4]);
std::string command(const std::string& line);
void status_text(char* out, size_t len);

}  // namespace rdrvr::deadeye
