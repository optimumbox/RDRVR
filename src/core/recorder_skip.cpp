#include "core/recorder_skip.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "core/anchors.h"
#include "core/camera_lever.h"
#include "core/config.h"
#include "core/hooks.h"
#include "core/log.h"

namespace rdrvr::recorder_skip {
namespace {

// The recorder's draw and state methods (recorder_skip.h): vtable byte offsets.
const uint16_t kSlots[] = {0xa0, 0xa8, 0xb8, 0xc0, 0xe0, 0x100, 0x140, 0x158, 0x168, 0x170, 0x178,
                           0x188, 0x190, 0x1a8, 0x1b8, 0x1e8, 0x1f0, 0x78, 0x80};
constexpr size_t kN = sizeof(kSlots) / sizeof(kSlots[0]);

// One block: the shared words, then a thunk per slot, then each slot's original.
//   thunk: mov eax, gs:[48h] (the thread id); cmp eax, [tid]; jne orig; cmp byte [on], 0; je orig; ret
//          orig: jmp [original]
// (the methods return nothing the callers use: void, the packet written or not)
struct Block {
    volatile uint32_t tid;    // the render thread's id while a second scene runs
    volatile uint8_t on;
    uint8_t pad[3];
    volatile uint64_t calls;  // (unused by the thunks: the skips are counted by begin/end)
    volatile uint8_t slot_on[kN];  // each method skipped (1) or recorded (0) in the second scene ("vi pass2 slots <mask>")
    void* orig[kN];
    uint8_t code[kN][48];
};
Block* g_b = nullptr;
bool g_installed = false;
std::atomic<uint64_t> g_scenes{0};

// The cuts ("vi pass2 cut <rva> on|off", [Stereo] ViSkipWalk): a game function that returns at once (a void one: its
// caller reads nothing back) while the second scene runs with the switch on, on the render thread; each through a
// thunk like the recorder's, so the arguments reach the original untouched otherwise.
//   thunk: mov eax, gs:[48h]; cmp eax, [tid]; jne orig; cmp byte [on], 0; je orig; cmp byte [cut], 0; je orig;
//          lock inc qword [n]; ret; orig: jmp [original]
struct Cut {
    uintptr_t rva;
    volatile uint8_t enabled;
    uint8_t pad[7];
    volatile uint64_t n;
    void* orig;
    uint8_t code[64];
};
constexpr int kMaxCuts = 8;
Cut* g_cuts = nullptr;  // (in the block's page: executable)
int g_ncuts = 0;

int32_t rel(const uint8_t* next, const volatile void* target) {
    return static_cast<int32_t>(reinterpret_cast<intptr_t>(target) - reinterpret_cast<intptr_t>(next));
}

}  // namespace

std::string cut(uintptr_t rva, bool on);

void install() {
    if (g_installed || !config::get_bool("Stereo", "ViMain", false) || !config::get_bool("Stereo", "ViSkipRecord", false) ||
        anchors::stand_down() || !anchors::present(anchors::Id::RecorderVtbl))
        return;
    void** vt = reinterpret_cast<void**>(anchors::addr(anchors::Id::RecorderVtbl));
    g_b = static_cast<Block*>(VirtualAlloc(nullptr, sizeof(Block), MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!g_b) return;
    std::memset(g_b, 0, sizeof(Block));
    // [Stereo] ViSkipSlots (hex, bit i = kSlots[i] skipped; default 0x6000: the draws only, +0x1a8 and +0x1b8). The
    // state methods are recorded: skipped, eye 1's distant cacti and grass flickered in eye 2 (2026-10-14, the player's
    // save; bisected in the headset: +0x100 and +0x140, the texture tables, copy a table's descriptors into the frame's
    // ring once a frame stamp, and more besides). Cost with ViSkipWalk: pass 2 0.47 -> 0.79 ms on the render thread.
    const uint32_t mask = static_cast<uint32_t>(std::strtoul(config::get_string("Stereo", "ViSkipSlots", "6000").c_str(), nullptr, 16));
    for (size_t i = 0; i < kN; ++i) g_b->slot_on[i] = (mask >> i) & 1;
    for (size_t i = 0; i < kN; ++i) {
        g_b->orig[i] = vt[kSlots[i] / 8];
        uint8_t* c = g_b->code[i];
        uint8_t* p = c;
        const uint8_t gs_tid[] = {0x65, 0x8b, 0x04, 0x25, 0x48, 0x00, 0x00, 0x00};  // mov eax, gs:[48h]
        std::memcpy(p, gs_tid, sizeof(gs_tid));
        p += sizeof(gs_tid);
        *p++ = 0x3b, *p++ = 0x05;  // cmp eax, [rip+tid]
        const int32_t r1 = rel(p + 4, &g_b->tid);
        std::memcpy(p, &r1, 4);
        p += 4;
        *p++ = 0x75, *p++ = 19;    // jne orig (+19: the cmp (7), je (2), cmp (7), je (2), ret (1))
        *p++ = 0x80, *p++ = 0x3d;  // cmp byte [rip+on], 0
        const int32_t r2 = rel(p + 5, &g_b->on);
        std::memcpy(p, &r2, 4);
        p += 4;
        *p++ = 0x00;
        *p++ = 0x74, *p++ = 10;    // je orig
        *p++ = 0x80, *p++ = 0x3d;  // cmp byte [rip+slot_on[i]], 0
        const int32_t r4 = rel(p + 5, &g_b->slot_on[i]);
        std::memcpy(p, &r4, 4);
        p += 4;
        *p++ = 0x00;
        *p++ = 0x74, *p++ = 0x01;  // je orig
        *p++ = 0xc3;               // ret
        *p++ = 0xff, *p++ = 0x25;  // orig: jmp [rip+orig[i]]
        const int32_t r3 = rel(p + 4, &g_b->orig[i]);
        std::memcpy(p, &r3, 4);
        p += 4;
    }
    FlushInstructionCache(GetCurrentProcess(), g_b, sizeof(Block));
    DWORD old = 0;
    void** first = vt + 0x78 / 8;
    const size_t span = (0x1f0 - 0x78) + 8;
    if (!VirtualProtect(first, span, PAGE_READWRITE, &old)) {
        log::warn("[recskip] the recorder's vtable not writable: not installed");
        return;
    }
    for (size_t i = 0; i < kN; ++i) vt[kSlots[i] / 8] = g_b->code[i];
    VirtualProtect(first, span, old, &old);
    g_installed = true;
    camera_lever::set_vi_skip2(true);  // (on from the start: "vi pass2 draw" records the second scene whole again)
    log::info("[recskip] %zu recorder methods through the second-scene switch (on; vi pass2 draw|skip)", kN);
    // [Stereo] ViSkipWalk (default on): the skipped scene's draw-list walk (SceneDrawWalk) returns at once. It recorded
    // nothing under the skip; its walk was ~55% of the second scene's render-thread time (2026-10-14, headset on the desk:
    // pass 2 1.61 -> 0.78 ms, both eyes' images unchanged). (Its caller FUN_1405cbfd0 cut instead lost eye 2's image.)
    if (config::get_bool("Stereo", "ViSkipWalk", true) && anchors::present(anchors::Id::SceneDrawWalk)) {
        const uintptr_t rva = anchors::addr(anchors::Id::SceneDrawWalk) - reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        log::info("[recskip] [Stereo] ViSkipWalk: %s", cut(rva, true).c_str());
    }
}

bool installed() { return g_installed; }

std::string cut(uintptr_t rva, bool on) {
    if (!g_installed) return "ERROR the recorder skip is not installed";
    for (int i = 0; i < g_ncuts; ++i)
        if (g_cuts[i].rva == rva) {
            g_cuts[i].enabled = on ? 1 : 0;
            return std::string("cut +") + std::to_string(rva) + (on ? " on" : " off") + " (decimal rva)";
        }
    if (!on) return "not cut";
    if (!g_cuts) {
        g_cuts = static_cast<Cut*>(VirtualAlloc(nullptr, sizeof(Cut) * kMaxCuts, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (!g_cuts) return "ERROR no memory";
        std::memset(g_cuts, 0, sizeof(Cut) * kMaxCuts);
    }
    if (g_ncuts >= kMaxCuts) return "ERROR too many cuts";
    Cut& c = g_cuts[g_ncuts];
    c.rva = rva;
    c.enabled = 1;
    uint8_t* p = c.code;
    const uint8_t gs_tid[] = {0x65, 0x8b, 0x04, 0x25, 0x48, 0x00, 0x00, 0x00};  // mov eax, gs:[48h]
    std::memcpy(p, gs_tid, sizeof(gs_tid));
    p += sizeof(gs_tid);
    *p++ = 0x3b, *p++ = 0x05;  // cmp eax, [rip+tid]
    int32_t r = rel(p + 4, &g_b->tid);
    std::memcpy(p, &r, 4);
    p += 4;
    uint8_t* j1 = p;
    *p++ = 0x75, *p++ = 0;     // jne orig
    *p++ = 0x80, *p++ = 0x3d;  // cmp byte [rip+on], 0
    r = rel(p + 5, &g_b->on);
    std::memcpy(p, &r, 4);
    p += 4;
    *p++ = 0x00;
    uint8_t* j2 = p;
    *p++ = 0x74, *p++ = 0;     // je orig
    *p++ = 0x80, *p++ = 0x3d;  // cmp byte [rip+enabled], 0
    r = rel(p + 5, &c.enabled);
    std::memcpy(p, &r, 4);
    p += 4;
    *p++ = 0x00;
    uint8_t* j3 = p;
    *p++ = 0x74, *p++ = 0;                // je orig
    *p++ = 0xf0, *p++ = 0x48, *p++ = 0xff, *p++ = 0x05;  // lock inc qword [rip+n]
    r = rel(p + 4, &c.n);
    std::memcpy(p, &r, 4);
    p += 4;
    *p++ = 0xc3;  // ret
    uint8_t* orig = p;
    *p++ = 0xff, *p++ = 0x25;  // orig: jmp [rip+orig]
    r = rel(p + 4, &c.orig);
    std::memcpy(p, &r, 4);
    p += 4;
    j1[1] = static_cast<uint8_t>(orig - (j1 + 2));
    j2[1] = static_cast<uint8_t>(orig - (j2 + 2));
    j3[1] = static_cast<uint8_t>(orig - (j3 + 2));
    FlushInstructionCache(GetCurrentProcess(), &c, sizeof(Cut));
    void* target = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) + rva);
    static char names[8][64];  // (the hook list keeps the name's pointer)
    char* name = names[g_ncuts & 7];
    std::snprintf(name, sizeof(names[0]), "RDR second-scene cut +%#llx", static_cast<unsigned long long>(rva));
    if (!hooks::install(name, target, c.code, &c.orig)) return "ERROR the hook failed";
    ++g_ncuts;
    return std::string("cut ") + name + " on";
}

std::string slots(uint32_t mask) {
    if (!g_installed) return "ERROR the recorder skip is not installed";
    std::string o = "second scene skips:";
    for (size_t i = 0; i < kN; ++i) {
        g_b->slot_on[i] = (mask >> i) & 1;
        char b[16];
        std::snprintf(b, sizeof(b), " %#x%s", kSlots[i], g_b->slot_on[i] ? "" : "(rec)");
        o += b;
    }
    return o;
}

void begin() {
    if (!g_installed) return;
    g_b->tid = GetCurrentThreadId();
    g_b->on = 1;
    g_scenes.fetch_add(1, std::memory_order_relaxed);
}
void end() {
    if (!g_installed) return;
    g_b->on = 0;
    g_b->tid = 0;
}

std::string status() {
    char b[160];
    std::snprintf(b, sizeof(b), "recorder skip %s: %llu second scenes recorded without draws", g_installed ? "installed" : "not installed",
                  static_cast<unsigned long long>(g_scenes.load()));
    std::string o = b;
    for (int i = 0; i < g_ncuts; ++i) {
        std::snprintf(b, sizeof(b), " | cut +%#llx %s, %llu returns", static_cast<unsigned long long>(g_cuts[i].rva),
                      g_cuts[i].enabled ? "on" : "off", static_cast<unsigned long long>(g_cuts[i].n));
        o += b;
    }
    return o;
}

}  // namespace rdrvr::recorder_skip
