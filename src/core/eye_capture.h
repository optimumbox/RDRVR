#pragma once
// Eye capture (single-pass stereo's eye-mismatch work, after KF2VR's -kf2vr-eye-capture): both eyes' images of one
// frame (eye_grab: the post output each eye's swapchain image is copied from, before the UI) saved together every N
// seconds, or on demand, as <dir>\eyes_<time>_<n>-L.rde / -R.rde. tools/sp/eyecap.py turns them into PNGs, a
// side-by-side, an anaglyph and a vertical-mismatch measure (a correct pair differs only horizontally).
//   [Debug] EyeCapture=<seconds> (0 off) and EyeCaptureDir=<folder> (default <game>\RDRVR_eyecap): from the start.
//   Test channel: "eyecap on [seconds] [dir]", "eyecap off", "eyecap now [dir]", "eyecap status".
//   In the headset: both trackpads held down (controllers.cpp) takes one pair (take_async).

#include <string>
#include <vector>

namespace rdrvr::eye_capture {

void init();  // the ini's switch: the capture thread
std::string command(const std::vector<std::string>& t);
// One pair into the capture folder on a thread of its own (the grab waits for the frames); false while one is running.
// done(ok) is called on that thread when it has finished.
bool take_async(void (*done)(bool ok));

}  // namespace rdrvr::eye_capture
