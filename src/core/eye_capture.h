#pragma once
// Eye capture (single-pass stereo's eye-mismatch work, after KF2VR's -kf2vr-eye-capture): both eyes' images of one
// frame (eye_grab: the post output each eye's swapchain image is copied from, before the UI) saved together every N
// seconds, or on demand, as <dir>\eyes_<time>_<n>-L.rde / -R.rde. tools/sp/eyecap.py turns them into PNGs, a
// side-by-side, an anaglyph and a vertical-mismatch measure (a correct pair differs only horizontally).
//   [Debug] EyeCapture=<seconds> (0 off) and EyeCaptureDir=<folder> (default <game>\RDRVR_eyecap): from the start.
//   Test channel: "eyecap on [seconds] [dir]", "eyecap off", "eyecap now [dir]", "eyecap status".

#include <string>
#include <vector>

namespace rdrvr::eye_capture {

void init();  // the ini's switch: the capture thread
std::string command(const std::vector<std::string>& t);

}  // namespace rdrvr::eye_capture
