#include "core/recorder_skip.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>

#include "core/anchors.h"
#include "core/camera_lever.h"
#include "core/config.h"
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
    void* orig[kN];
    uint8_t code[kN][48];
};
Block* g_b = nullptr;
bool g_installed = false;
std::atomic<uint64_t> g_scenes{0};

int32_t rel(const uint8_t* next, const volatile void* target) {
    return static_cast<int32_t>(reinterpret_cast<intptr_t>(target) - reinterpret_cast<intptr_t>(next));
}

}  // namespace

void install() {
    if (g_installed || !config::get_bool("Stereo", "ViMain", false) || !config::get_bool("Stereo", "ViSkipRecord", true) ||
        anchors::stand_down() || !anchors::present(anchors::Id::RecorderVtbl))
        return;
    void** vt = reinterpret_cast<void**>(anchors::addr(anchors::Id::RecorderVtbl));
    g_b = static_cast<Block*>(VirtualAlloc(nullptr, sizeof(Block), MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!g_b) return;
    std::memset(g_b, 0, sizeof(Block));
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
        *p++ = 0x75, *p++ = 10;    // jne orig (+10: the cmp (7), je (2), ret (1))
        *p++ = 0x80, *p++ = 0x3d;  // cmp byte [rip+on], 0
        const int32_t r2 = rel(p + 5, &g_b->on);
        std::memcpy(p, &r2, 4);
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
}

bool installed() { return g_installed; }

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
    return b;
}

}  // namespace rdrvr::recorder_skip
