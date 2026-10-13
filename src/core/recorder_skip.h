#pragma once
// [Stereo] ViSkipRecord: a view-instanced frame's second scene records no draws and no draw state. Slice 0's region drew
// both eyes (stereo_vi.h), so the second scene's draws would all be skipped at playback anyway; recording them cost the
// render thread its whole second walk's packets, and replaying their state the playback thread ~11 Mcycles a frame
// (2026-10-13), the skip at the D3D12 calls saving neither. The scene itself still runs (its clears, target binds,
// copies, queries and the mod's markers are recorded): only the recorder's draw and state methods return at once, on
// the render thread, while camera_lever has the switch on. Skipping the whole scene instead left the frame's
// completion unsignalled (the main thread waited for ever, 2026-10-13).
//
// The recorder (RecorderVtbl) methods by the packet each writes (research\recorder_ops.txt): +0xa0 9 pipeline, +0xa8 0xb,
// +0xb8 0xd, +0xc0 0xe, +0xe0 0x12, +0x100/+0x140 0xa (texture tables), +0x158 0x26, +0x168 0x24, +0x170 0x27,
// +0x178 0x2a, +0x188 0x2b, +0x190 0x2c, +0x1a8 0x2f, +0x1b8 0x32 (draws), +0x1e8 0x41, +0x1f0 0x28, +0x78 7, +0x80 8.

#include <cstdint>
#include <string>

namespace rdrvr::recorder_skip {

void install();               // with [Stereo] ViMain: the vtable entries through the thunks (off until begin)
bool installed();
void begin();                 // the render thread: its recorder's draw and state methods return at once
void end();
std::string status();

}  // namespace rdrvr::recorder_skip
