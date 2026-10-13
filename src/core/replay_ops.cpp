#include "core/replay_ops.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>

#include "core/anchors.h"
#include "core/config.h"
#include "core/hooks.h"
#include "core/log.h"

extern "C" void rdrvr_ro_stub();
extern "C" void* rdrvr_ro_back = nullptr;  // ReplayDispatch + 5: the dispatch's ja (flags of the cmp the stub repeats)
extern "C" void* rdrvr_ro_tail = nullptr;  // ReplayChunk + 0x7ef: the common tail (add [r14], eax)

namespace rdrvr::replay_ops {
namespace {

constexpr int kOps = 0x51;  // opcodes 1..0x50
constexpr uint32_t kTailRva = 0x7ef;
bool g_installed = false;
// the playback thread's (one writer; the status reads them torn at worst)
thread_local uint32_t g_cur = 0;
bool g_r2 = false;
DWORD g_r2_tid = 0;  // the thread playing the region (other threads' packets are not its)
struct Tid {
    DWORD id = 0;
    uint64_t packets = 0;
};
Tid g_tids[8];
uint64_t g_count[2][kOps]{}, g_skipped[kOps]{};
uint64_t g_sem[2][kOps][kKinds]{};
int g_size[kOps]{};  // 0 not seen, > 0 every one seen, -1 sizes differ
thread_local const int* g_last_off = nullptr;
thread_local int g_last_val = 0;
thread_local uint32_t g_last_op = 0;
std::atomic<bool> g_skip[kOps]{};
bool g_marker_op[kOps]{};  // ops seen running the mod's markers: never skipped

// The opcodes skipped by default: none until the profile says which are state setup only ("ops skip default" then).
const uint8_t kDefaultSkip[] = {0};

}  // namespace

void note(Kind k) {
    if (!g_installed) return;
    const uint32_t op = g_cur < kOps ? g_cur : 0;
    ++g_sem[region2() ? 1 : 0][op][k];
    if (k == kMarker) g_marker_op[op] = true;
}
void set_region2(bool on) {
    g_r2_tid = on ? GetCurrentThreadId() : 0;
    g_r2 = on;
}
bool region2() { return g_r2 && g_r2_tid == GetCurrentThreadId(); }

}  // namespace rdrvr::replay_ops

extern "C" uint32_t rdrvr_ro_dispatch(uint32_t op, const void*, const int* off) {
    using namespace rdrvr::replay_ops;
    const DWORD tid = GetCurrentThreadId();
    for (Tid& t : g_tids)  // (racy between threads: a count, nothing depends on it)
        if (t.id == tid || t.id == 0) {
            t.id = tid;
            ++t.packets;
            break;
        }
    const bool r2 = g_r2 && g_r2_tid == tid;
    if (off == g_last_off && g_last_op && g_last_op != 0x40 && g_last_op < kOps) {
        const int d = *off - g_last_val;
        if (d > 0 && d <= 0x400) {
            int& s = g_size[g_last_op];
            if (s == 0) s = d;
            else if (s != d) s = -1;
        }
    }
    g_last_off = off;
    g_last_val = *off;
    g_last_op = op;
    g_cur = op;
    if (op >= kOps) return 0;
    ++g_count[r2 ? 1 : 0][op];
    if (r2 && op != 0x40 && op != 0x50 && !g_marker_op[op] && g_size[op] > 0 && g_skip[op].load(std::memory_order_relaxed)) {
        ++g_skipped[op];
        return static_cast<uint32_t>(g_size[op]);
    }
    return 0;
}

namespace rdrvr::replay_ops {

void install() {
    if (!config::get_bool("Stereo", "ViSkipPackets", false) || anchors::stand_down()) return;
    if (!anchors::present(anchors::Id::ReplayDispatch) || !anchors::present(anchors::Id::ReplayChunk)) {
        log::warn("[ops] not installed: no ReplayDispatch/ReplayChunk on this build");
        return;
    }
    const uintptr_t site = anchors::addr(anchors::Id::ReplayDispatch), tail = anchors::addr(anchors::Id::ReplayChunk) + kTailRva;
    const uint8_t want_site[5] = {0xff, 0xc8, 0x83, 0xf8, 0x4f}, want_tail[3] = {0x41, 0x01, 0x06};  // dec eax; cmp eax,4Fh / add [r14],eax
    if (std::memcmp(reinterpret_cast<const void*>(site), want_site, 5) != 0 || std::memcmp(reinterpret_cast<const void*>(tail), want_tail, 3) != 0) {
        log::warn("[ops] not installed: the dispatch or the tail is not as expected (or already patched)");
        return;
    }
    rdrvr_ro_back = reinterpret_cast<void*>(site + 5);
    rdrvr_ro_tail = reinterpret_cast<void*>(tail);
    void* tramp = nullptr;
    if (!hooks::install("RDR ReplayChunk dispatch (ViSkipPackets)", reinterpret_cast<void*>(site), reinterpret_cast<void*>(&rdrvr_ro_stub), &tramp))
        return;
    g_installed = true;
    log::info("[ops] the playback dispatch counts the packets (eye 2's region of a view-instanced frame: \"ops skip\")");
}

std::string command(const std::vector<std::string>& t) {
    if (!g_installed) return "ERROR not installed ([Stereo] ViSkipPackets=1)";
    const std::string sub = t.size() >= 2 ? t[1] : "";
    if (sub == "reset") {
        std::memset(g_count, 0, sizeof(g_count));
        std::memset(g_sem, 0, sizeof(g_sem));
        std::memset(g_skipped, 0, sizeof(g_skipped));
        return "reset";
    }
    if (sub == "skip") {
        for (size_t i = 2; i < t.size(); ++i) {
            if (t[i] == "none") {
                for (auto& s : g_skip) s = false;
            } else if (t[i] == "default") {
                for (uint8_t o : kDefaultSkip)
                    if (o) g_skip[o] = true;
            } else {
                const unsigned long o = std::strtoul(t[i].c_str(), nullptr, 16);
                if (o > 0 && o < static_cast<unsigned long>(kOps)) g_skip[o] = true;
            }
        }
    }
    // per opcode, most played in eye 2's regions first: plays (other / eye 2's region), size, skipped, the D3D12 calls
    static const char* const kNames[kKinds] = {"draw", "pso", "barrier", "clear", "copy", "query", "pred", "targets", "table", "rootcbv",
                                               "heaps", "indirect", "cbv", "MARKER"};
    std::vector<int> ops;
    for (int o = 1; o < kOps; ++o)
        if (g_count[0][o] || g_count[1][o]) ops.push_back(o);
    std::sort(ops.begin(), ops.end(), [](int a, int b) { return g_count[1][a] > g_count[1][b]; });
    std::string r = "threads:";
    for (const Tid& t : g_tids)
        if (t.id) r += " " + std::to_string(t.id) + " (" + std::to_string(t.packets) + ")";
    r += "; op: plays other/eye2, size, skip, calls(other/eye2);";
    for (int o : ops) {
        char b[96];
        std::snprintf(b, sizeof(b), " %02x: %llu/%llu sz%d%s", o, static_cast<unsigned long long>(g_count[0][o]),
                      static_cast<unsigned long long>(g_count[1][o]), g_size[o], g_skip[o] ? (g_marker_op[o] ? " SKIP(marker: no)" : " SKIP") : "");
        r += b;
        for (int k = 0; k < kKinds; ++k)
            if (g_sem[0][o][k] || g_sem[1][o][k]) {
                std::snprintf(b, sizeof(b), " %s %llu/%llu", kNames[k], static_cast<unsigned long long>(g_sem[0][o][k]),
                              static_cast<unsigned long long>(g_sem[1][o][k]));
                r += b;
            }
        if (g_skipped[o]) {
            std::snprintf(b, sizeof(b), " skipped %llu", static_cast<unsigned long long>(g_skipped[o]));
            r += b;
        }
        r += ";";
    }
    return r;
}

}  // namespace rdrvr::replay_ops
