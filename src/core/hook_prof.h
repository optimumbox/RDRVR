#pragma once
// "hookprof on|off|reset": the time the mod's command-list hooks take, per hook, split into the hook's own work and the
// D3D12 call it makes (rdtsc cycles; the playback thread makes nearly all of these calls). What single-pass stereo adds
// to the playback thread, against the two-pass build's.

#include <intrin.h>

#include <atomic>
#include <cstdint>
#include <string>

namespace rdrvr::hookprof {

enum Id {
    kDraw, kPso, kTargets, kBarrier, kTable, kHeaps, kViewport, kScissor, kClear, kCopy, kQuery, kPredication, kReset,
    kCount
};
inline const char* const kNames[kCount] = {"draw",  "pso",  "targets", "barrier", "table", "heaps", "viewport",
                                           "scissor", "clear", "copy", "query", "predication", "reset"};

inline std::atomic<bool> g_on{false};
struct Acc {
    std::atomic<uint64_t> calls{0}, total{0}, orig{0};
};
inline Acc g_acc[kCount];

struct Scope {
    int id;
    uint64_t t0, orig = 0;
    explicit Scope(int i) : id(i), t0(g_on.load(std::memory_order_relaxed) ? __rdtsc() : 0) {}
    ~Scope() {
        if (!t0) return;
        const uint64_t t = __rdtsc() - t0;
        g_acc[id].calls.fetch_add(1, std::memory_order_relaxed);
        g_acc[id].total.fetch_add(t, std::memory_order_relaxed);
        g_acc[id].orig.fetch_add(orig, std::memory_order_relaxed);
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};
// the D3D12 call inside a hook: HP_ORIG(scope, o_X(...));
#define HP_ORIG(scope, ...)                                         \
    do {                                                            \
        const uint64_t hp_a_ = (scope).t0 ? __rdtsc() : 0;          \
        __VA_ARGS__;                                                \
        if (hp_a_) (scope).orig += __rdtsc() - hp_a_;               \
    } while (0)

std::string command(const std::string& sub);  // hook_prof.cpp

}  // namespace rdrvr::hookprof
