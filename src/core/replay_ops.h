#pragma once
// The playback loop's packets by opcode (ReplayChunk's 0x50 cases, docs/SINGLE_PASS_PLAN.md "Findings"): counted, each
// opcode's size learnt from the read offset's advance, and the D3D12 calls each one makes (the hooks note them against
// the packet being played). In a view-instanced frame's second region (eye 2's pass, its draws drawn with eye 1's:
// stereo_vi.h) the opcodes switched on with "ops skip" are not played at all ([Stereo] ViSkipPackets): the game's state
// setup for draws that will be skipped costs the playback thread as much as the draws (2026-10-13: 11.4 Mcycles a frame).
//
// [Stereo] ViSkipPackets=1 installs it (with ViMain). Test channel: "ops [reset]", "ops skip <op> ...|none|default".

#include <cstdint>
#include <string>
#include <vector>

namespace rdrvr::replay_ops {

enum Kind : int { kDraw, kPso, kBarrier, kClear, kCopy, kQuery, kPredication, kTargets, kTable, kRootCbv, kHeaps, kIndirect,
                  kCreateCbv, kMarker, kKinds };
void install();
void note(Kind k);       // a D3D12 call (or one of the mod's markers) made by the packet now played
void set_region2(bool);  // the playback thread: a view-instanced frame's second region began / ends
bool region2();
std::string command(const std::vector<std::string>& t);

}  // namespace rdrvr::replay_ops
