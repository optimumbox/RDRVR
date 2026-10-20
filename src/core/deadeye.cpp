#include "core/deadeye.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <sstream>

#include "core/aim.h"
#include "core/anchors.h"
#include "core/config.h"
#include "core/hooks.h"
#include "core/log.h"
#include "core/ui_layer.h"

namespace rdrvr::deadeye {
namespace {

// FUN_140394300 (T, viewport): for each mark, MarkWorldPos, then (the DeadeyeLocks path) ViewportPointVisible shows or
// hides its element and ScreenToUi places it; the duel's path (FUN_140d07680 per mark) calls ScreenToUi too. One
// ScreenToUi per mark, after its MarkWorldPos, so the k-th of each is the k-th X.
using MarksUi_t = void (*)(uintptr_t T, uintptr_t vp);
using WorldPos_t = void (*)(uintptr_t mark, float* out);
using Visible_t = uint8_t (*)(uintptr_t vp, const float* p);
using ToUi_t = float* (*)(float* out, const float* in);
MarksUi_t o_marks_ui = nullptr;
WorldPos_t o_world_pos = nullptr;
Visible_t o_visible = nullptr;
ToUi_t o_to_ui = nullptr;

std::atomic<bool> g_on{true};
std::atomic<int> g_pitch{200};     // the slots' spacing (UI pixels): each slot's crop
std::atomic<bool> g_barrel{true};  // [Hands] DeadEyeBarrelPaint: marks painted where the barrel's probe is, not by the game's assisted pick
std::atomic<float> g_deg{3.0f};    // the crop's width in the eyes (degrees)
std::atomic<int> g_off[2] = {0, 0};  // the X's centre from the element's point (UI pixels), measured

thread_local int t_k = -1;  // inside the update: the mark being placed
thread_local float t_pos[kMax][3];

std::mutex g_mutex;
Mark g_marks[kMax];
int g_n = 0;
double g_ms = 0;
std::atomic<uint64_t> g_updates{0}, g_placed{0};

double now_ms() { return static_cast<double>(GetTickCount64()); }

void slot_of(int k, float* out) {
    uint32_t w = 0, h = 0;
    ui_layer::size(&w, &h);
    const int p = g_pitch.load(std::memory_order_relaxed);
    const int per_row = w > static_cast<uint32_t>(p) ? static_cast<int>(w) / p : 1;
    out[0] = static_cast<float>(p) * (0.5f + static_cast<float>(k % per_row));
    out[1] = static_cast<float>(p) * (0.5f + static_cast<float>(k / per_row));
}

// FUN_14034bc00, the reticle's and Dead Eye's UI each tick, runs them only while channel 0's camera is the gameplay
// camera; in VR it is the anchor's (pose.cpp), so the X were never placed. While Dead Eye is on or marks are painted,
// channel 0 holds the gameplay camera for its call (as aim.cpp's hk_fire does for the trigger).
using ReticleHud_t = void (*)(uintptr_t info, uintptr_t a2, uintptr_t a3, uintptr_t a4);
ReticleHud_t o_reticle_hud = nullptr;
std::atomic<uint64_t> g_hud_calls{0}, g_hud_swaps{0};

template <class T>
bool rd(uintptr_t a, T* out) {
    if (!a) return false;
    __try {
        *out = *reinterpret_cast<const T*>(a);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
template <class T>
bool wr(uintptr_t a, T v) {
    __try {
        *reinterpret_cast<T*>(a) = v;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool dead_eye_or_marks() {
    uintptr_t info = 0, R = 0;
    int32_t mode = 0;
    if (rd(anchors::addr(anchors::Id::PlayerInfo), &info) && info && rd(info + 0x410, &R) && R && rd(R + 0x1c, &mode) && mode != 0) return true;
    std::lock_guard lock(g_mutex);
    return g_n > 0 && now_ms() - g_ms < 500.0;
}

void hk_reticle_hud(uintptr_t info, uintptr_t a2, uintptr_t a3, uintptr_t a4) {
    g_hud_calls.fetch_add(1, std::memory_order_relaxed);
    uintptr_t cm = 0, chs = 0, ch = 0, cur = 0, gp = 0;
    if (!g_on.load(std::memory_order_relaxed) || !dead_eye_or_marks() || !rd(anchors::addr(anchors::Id::CamManager), &cm) || !cm ||
        !rd(cm + 8, &chs) || !chs || !rd(chs + 0x18, &ch) || !ch || !rd(ch + 0x10, &cur) || !rd(cm + 0x28, &gp) || !gp || cur == gp) {
        o_reticle_hud(info, a2, a3, a4);
        return;
    }
    aim::camera_swap_flag(true);
    if (!wr<uintptr_t>(ch + 0x10, gp)) {
        aim::camera_swap_flag(false);
        o_reticle_hud(info, a2, a3, a4);
        return;
    }
    o_reticle_hud(info, a2, a3, a4);
    wr<uintptr_t>(ch + 0x10, cur);
    aim::camera_swap_flag(false);
    g_hud_swaps.fetch_add(1, std::memory_order_relaxed);
}

void hk_marks_ui(uintptr_t T, uintptr_t vp) {
    if (!g_on.load(std::memory_order_relaxed)) {
        o_marks_ui(T, vp);
        return;
    }
    t_k = 0;
    o_marks_ui(T, vp);
    const int n = t_k < kMax ? t_k : kMax;
    t_k = -1;
    std::lock_guard lock(g_mutex);
    for (int k = 0; k < n; ++k) {
        std::memcpy(g_marks[k].world, t_pos[k], sizeof(g_marks[k].world));
        slot_of(k, g_marks[k].slot);
        g_marks[k].slot[0] += static_cast<float>(g_off[0].load(std::memory_order_relaxed));
        g_marks[k].slot[1] += static_cast<float>(g_off[1].load(std::memory_order_relaxed));
    }
    g_n = n;
    g_ms = now_ms();
    g_updates.fetch_add(1, std::memory_order_relaxed);
}

void hk_world_pos(uintptr_t mark, float* out) {
    o_world_pos(mark, out);
    if (t_k >= 0 && t_k < kMax) std::memcpy(t_pos[t_k], out, sizeof(t_pos[t_k]));
}

uint8_t hk_visible(uintptr_t vp, const float* p) {
    const uint8_t r = o_visible(vp, p);
    return t_k >= 0 ? 1 : r;  // inside: shown wherever the camera looks
}

float* hk_to_ui(float* out, const float* in) {
    if (t_k < 0) return o_to_ui(out, in);
    float s[2];
    slot_of(t_k < kMax ? t_k : kMax - 1, s);
    ++t_k;
    g_placed.fetch_add(1, std::memory_order_relaxed);
    return o_to_ui(out, s);
}

// Painting a mark: the trigger (FireTrigger) asks with T +0x6084, and the next TargetingUpdate adds the mark
// (0x14039a3f0). The trigger also sets +0x6085 (and +0x6086) when the game's targeting is assisted (the player's
// +0x3be bit 2 clear); then the update leaves the probe's actor and marks a target it picks itself (0x140395bb0, from
// what the flat camera sees, with the soft lock's tuning at +0x57c0 that aim.cpp clears; refused with +0x57e6 when it
// finds none), so in VR a mark often never came where the barrel pointed. Cleared, the update marks the probe's actor:
// the one on the barrel's ray (the AimRay override).
using TargetingUpdate_t = void (*)(uintptr_t T, uintptr_t a2, uintptr_t a3, uintptr_t a4);
TargetingUpdate_t o_targeting = nullptr;

void hk_targeting(uintptr_t T, uintptr_t a2, uintptr_t a3, uintptr_t a4) {
    uint8_t req = 0, assisted = 0;
    if (g_barrel.load(std::memory_order_relaxed) && rd(T + 0x6084, &req) && req && rd(T + 0x6085, &assisted) && assisted && dead_eye_or_marks())
        wr<uint8_t>(T + 0x6085, 0);
    o_targeting(T, a2, a3, a4);
}

}  // namespace

void init() {
    g_on = config::get_bool("Hands", "DeadEyeMarks", true);
    g_deg = config::get_float("Hands", "DeadEyeMarkDegrees", 3.0f);
    g_barrel = config::get_bool("Hands", "DeadEyeBarrelPaint", true);
}

void install() {
    using anchors::Id;
    if (g_barrel.load()) {
        hooks::install("RDR targeting update (deadeye)", reinterpret_cast<void*>(anchors::addr(Id::TargetingUpdate)), hk_targeting, &o_targeting);
    }
    log::info("[deadeye] marks painted %s", g_barrel.load() ? "on the barrel's target (the game's assisted pick off in Dead Eye)" : "as the game picks");
    if (!g_on.load()) return;
    bool ok = hooks::install("RDR reticle UI update (deadeye)", reinterpret_cast<void*>(anchors::addr(Id::ReticleHudUpdate)), hk_reticle_hud, &o_reticle_hud);
    ok = ok && hooks::install("RDR Dead Eye marks' UI (deadeye)", reinterpret_cast<void*>(anchors::addr(Id::DeadeyeMarksUi)), hk_marks_ui, &o_marks_ui);
    ok = ok && hooks::install("RDR mark world point (deadeye)", reinterpret_cast<void*>(anchors::addr(Id::MarkWorldPos)), hk_world_pos, &o_world_pos);
    ok = ok && hooks::install("RDR viewport point test (deadeye)", reinterpret_cast<void*>(anchors::addr(Id::ViewportPointVisible)), hk_visible, &o_visible);
    ok = ok && hooks::install("RDR screen to UI (deadeye)", reinterpret_cast<void*>(anchors::addr(Id::ScreenToUi)), hk_to_ui, &o_to_ui);
    if (!ok) g_on = false;
    log::info("[deadeye] the marks' X %s", g_on.load() ? "shown in the eyes (the game's own, from the UI)" : "as the game's (off)");
}

bool enabled() { return g_on.load(std::memory_order_relaxed); }

int marks(Mark* out, int max, float* crop_px, float* deg) {
    *crop_px = static_cast<float>(g_pitch.load(std::memory_order_relaxed));
    *deg = g_deg.load(std::memory_order_relaxed);
    if (!g_on.load(std::memory_order_relaxed)) return 0;
    std::lock_guard lock(g_mutex);
    if (now_ms() - g_ms > 200.0) return 0;
    const int n = g_n < max ? g_n : max;
    std::memcpy(out, g_marks, sizeof(Mark) * static_cast<size_t>(n));
    return n;
}

bool band(int out[4]) {
    if (!g_on.load(std::memory_order_relaxed)) return false;
    int n = 0;
    {
        std::lock_guard lock(g_mutex);
        if (now_ms() - g_ms > 400.0) return false;
        n = g_n;
    }
    if (n <= 0) return false;
    uint32_t w = 0, h = 0;
    ui_layer::size(&w, &h);
    const int p = g_pitch.load(std::memory_order_relaxed);
    const int per_row = w > static_cast<uint32_t>(p) ? static_cast<int>(w) / p : 1;
    out[0] = 0;
    out[1] = 0;
    out[2] = static_cast<int>(w);
    out[3] = p * ((kMax + per_row - 1) / per_row);
    return true;
}

std::string command(const std::string& line) {
    // deadeye [on|off] [barrel on|off] [pitch <px>] [deg <d>] [shift <x> <y>]
    std::istringstream in(line);
    std::string c, w;
    in >> c;
    while (in >> w) {
        if (w == "on") g_on = true;
        else if (w == "off") g_on = false;
        else if (w == "barrel") {
            std::string v;
            if (in >> v) g_barrel = v == "on" || v == "1";
        } else if (w == "pitch") {
            int v = 0;
            if (in >> v && v >= 32 && v <= 600) g_pitch = v;
        } else if (w == "deg") {
            float v = 0;
            if (in >> v && v > 0.2f && v < 30.0f) g_deg = v;
        } else if (w == "shift") {
            int x = 0, y = 0;
            if (in >> x >> y) {
                g_off[0] = x;
                g_off[1] = y;
            }
        }
    }
    char b[400];
    status_text(b, sizeof(b));
    std::string o = b;
    std::lock_guard lock(g_mutex);
    for (int k = 0; k < g_n && k < 8; ++k) {
        std::snprintf(b, sizeof(b), " | %d at (%.2f %.2f %.2f) slot (%.0f %.0f)", k, g_marks[k].world[0], g_marks[k].world[1], g_marks[k].world[2],
                      g_marks[k].slot[0], g_marks[k].slot[1]);
        o += b;
    }
    return o;
}

void status_text(char* out, size_t len) {
    double age = 0;
    int n = 0;
    {
        std::lock_guard lock(g_mutex);
        age = now_ms() - g_ms;
        n = g_n;
    }
    std::snprintf(out, len, "deadeye %s barrel %s pitch %d deg %.1f shift (%d %d) | hud calls %llu swaps %llu | updates %llu placed %llu | last %d marks %.0f ms ago", g_on.load() ? "on" : "off",
                  g_barrel.load() ? "on" : "off", g_pitch.load(), g_deg.load(), g_off[0].load(), g_off[1].load(), static_cast<unsigned long long>(g_hud_calls.load()),
                  static_cast<unsigned long long>(g_hud_swaps.load()), static_cast<unsigned long long>(g_updates.load()),
                  static_cast<unsigned long long>(g_placed.load()), n, age);
}

}  // namespace rdrvr::deadeye
