#include "core/hogtie.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <thread>

#include "common/rdrvr_api.h"
#include "core/api.h"
#include "core/config.h"
#include "core/held_prop.h"
#include "core/log.h"

namespace rdrvr::hogtie {
namespace {

// The natives (MagicRDR's natives.json; each hash checked against its name's joaat)
constexpr uint32_t kGetLassoTarget = 0xAA364907u;     // GET_LASSO_TARGET(user) -> the roped actor
constexpr uint32_t kCanHogtie = 0x1AA3A0C0u;          // CAN_ACTOR_HOGTIE_TARGET(actor, target) (logged only)
constexpr uint32_t kIsHogtied = 0xA610DC79u;          // IS_ACTOR_HOGTIED(actor)
constexpr uint32_t kHogtieActor = 0x4440BCA5u;        // HOGTIE_ACTOR(actor)
constexpr uint32_t kGetPosition = 0x99BD9D6Fu;        // GET_POSITION(actor, Vector3*)
// HogtieWatch: why the game's own CAN_ACTOR_HOGTIE_TARGET says 0 in VR (its check, RDR.exe 140ca9ae0, probes the
// ground and the two actors' places and facing)
constexpr uint32_t kGetHeading = 0x42DE39F0u;         // GET_HEADING(actor) -> float
constexpr uint32_t kPosture = 0xDB993A4Fu;            // GET_ACTOR_POSTURE(actor)
constexpr uint32_t kOnGround = 0x709EC06Cu;           // IS_ACTOR_ON_GROUND(actor)
constexpr uint32_t kRagdoll = 0x3918D335u;            // IS_ACTOR_RAGDOLL(actor)
constexpr uint32_t kWaitMs = 500;                     // a native's result: the next script tick or two
constexpr int kGameFirstMs = 400;                     // the game's own hogtie given that long to start

std::atomic<bool> g_on{false};
std::atomic<float> g_range{3.0f};  // [Gestures] ForceHogtieRange (m)
std::mutex g_mutex;
std::condition_variable g_cv;
bool g_press = false;  // under g_mutex
std::atomic<uint64_t> g_forced{0};
std::atomic<bool> g_watch{false};  // [Gestures] HogtieWatch: the lasso's victim and the game's hogtie check logged

bool call(uint32_t hash, std::initializer_list<uint64_t> a, RdrvrNativeResult* r, uint32_t vec_out = 0) {
    uint64_t args[8] = {};
    uint32_t n = 0;
    for (uint64_t v : a) args[n++] = v;
    const uint64_t id = api::queue_native(hash, args, n, vec_out);
    if (api::wait_native(id, r, kWaitMs)) return true;
    api::cancel_op(id);
    return false;
}

uint64_t h(int32_t actor) { return static_cast<uint64_t>(static_cast<uint32_t>(actor)); }

bool hogtied(int32_t t) {
    RdrvrNativeResult r{};
    return call(kIsHogtied, {h(t)}, &r) && (r.value & 0xff) != 0;
}

uint32_t value(uint32_t hash, std::initializer_list<uint64_t> a) {
    RdrvrNativeResult r{};
    return call(hash, a, &r) ? static_cast<uint32_t>(r.value) : 0xFFFFFFFFu;
}

float fvalue(uint32_t hash, std::initializer_list<uint64_t> a) {
    RdrvrNativeResult r{};
    if (!call(hash, a, &r)) return -999.0f;
    float f;
    const uint32_t u = static_cast<uint32_t>(r.value);
    std::memcpy(&f, &u, 4);
    return f;
}

// One line about the player and the one on the lasso: false with nobody on it.
bool watch_line(const char* why) {
    RdrvrActorState st{};
    if (!api::actor_state(&st) || !st.valid) return false;
    const int32_t p = st.actor;
    const uint32_t tv = value(kGetLassoTarget, {h(p)});
    const int32_t t = static_cast<int32_t>(tv);
    if (t == 0 || t == -1 || tv == 0xFFFFFFFFu) return false;
    RdrvrNativeResult pp{}, tp{};
    if (!call(kGetPosition, {h(p), 0}, &pp, 2) || !call(kGetPosition, {h(t), 0}, &tp, 2)) return false;
    const float dx = tp.vec[0] - pp.vec[0], dy = tp.vec[1] - pp.vec[1], dz = tp.vec[2] - pp.vec[2];
    const float bearing = std::atan2(dx, dz) * 57.29578f, bearing_xy = std::atan2(dx, dy) * 57.29578f;
    log::info("[hogtie] watch %s: victim %d | %.2f m (dx %.2f dy %.2f dz %.2f, bearing xz %.0f xy %.0f) | the player: heading %.1f posture %u weapon %d flags %#x | the victim: heading %.1f posture %u ragdoll %u on ground %u hogtied %u | CAN_ACTOR_HOGTIE_TARGET(player, victim) %u",
              why, t, std::sqrt(dx * dx + dy * dy + dz * dz), dx, dy, dz, bearing, bearing_xy, fvalue(kGetHeading, {h(p)}),
              value(kPosture, {h(p)}), st.weapon, st.flags, fvalue(kGetHeading, {h(t)}), value(kPosture, {h(t)}),
              value(kRagdoll, {h(t)}) & 0xff, value(kOnGround, {h(t)}) & 0xff, value(kIsHogtied, {h(t)}) & 0xff,
              value(kCanHogtie, {h(p), h(t)}));
    return true;
}

// Someone on the player's lasso (on foot): the mod's free props (held_prop: the guns at the holsters and the rest)
// cleared until the rope is empty for a second. 2026-10-16: the game's own "press Y to hogtie" never started in VR:
// its check (CAN_ACTOR_HOGTIE_TARGET, RDR.exe 140ca9ae0) sweeps a 0.5 m capsule between the two and refuses on any
// hit but the two actors and what is attached to them, and the holster's gun at the hip, a free object, was hit (with
// it hidden the check said 1 and the game hogtied with its animation: the player the hogtie's master, which a mission
// waits for; HOGTIE_ACTOR's hogtie has none).
void watcher() {
    int empty = 0;
    bool clear = false;
    for (int n = 0;; ++n) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        if (g_watch.load(std::memory_order_relaxed) && n % 2 == 0) watch_line("tick");
        RdrvrActorState st{};
        if (!api::actor_state(&st) || !st.valid) continue;
        RdrvrNativeResult r{};
        if (!call(kGetLassoTarget, {h(st.actor)}, &r)) continue;  // no script tick (paused): kept as it is
        const int32_t t = static_cast<int32_t>(static_cast<uint32_t>(r.value));
        const bool roped = t != 0 && t != -1 && t != st.actor && !(st.flags & (RDRVR_ACTOR_MOUNTED | RDRVR_ACTOR_DRIVING));
        empty = roped ? 0 : empty + 1;
        const bool want = roped || (clear && empty < 4);
        if (want != clear) {
            clear = want;
            held_prop::set_clear(clear);
        }
    }
}

void attempt() {
    RdrvrActorState st{};
    if (!api::actor_state(&st) || !st.valid || (st.flags & (RDRVR_ACTOR_MOUNTED | RDRVR_ACTOR_DRIVING))) return;  // on foot only
    RdrvrNativeResult r{};
    if (!call(kGetLassoTarget, {h(st.actor)}, &r)) return;
    const int32_t t = static_cast<int32_t>(static_cast<uint32_t>(r.value));
    if (t == 0 || t == -1 || t == st.actor) return;  // nobody on the rope
    if (hogtied(t)) return;
    RdrvrNativeResult pp{}, tp{};
    if (!call(kGetPosition, {h(st.actor), 0}, &pp, 2) || !call(kGetPosition, {h(t), 0}, &tp, 2)) return;
    const float dx = tp.vec[0] - pp.vec[0], dy = tp.vec[1] - pp.vec[1], dz = tp.vec[2] - pp.vec[2];
    const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    RdrvrNativeResult can{};
    const bool can_ok = call(kCanHogtie, {h(st.actor), h(t)}, &can);
    if (dist > g_range.load(std::memory_order_relaxed)) {
        log::info("[hogtie] Y with %d on the lasso, %.1f m away: too far (ForceHogtieRange %.1f)", t, dist, g_range.load());
        return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(kGameFirstMs));
    if (hogtied(t)) {
        log::info("[hogtie] Y with %d on the lasso, %.1f m away: the game hogtied them itself", t, dist);
        return;
    }
    if (g_watch.load()) watch_line("Y");
    RdrvrNativeResult done{};
    call(kHogtieActor, {h(t)}, &done);
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    const bool after = hogtied(t);
    g_forced.fetch_add(1, std::memory_order_relaxed);
    log::info("[hogtie] Y with %d on the lasso, %.1f m away: HOGTIE_ACTOR sent (the game's CAN_ACTOR_HOGTIE_TARGET %s %llu); hogtied a second later %d",
              t, dist, can_ok ? "said" : "gave no answer,", static_cast<unsigned long long>(can.value & 0xffffffffu), after ? 1 : 0);
}

void worker() {
    for (;;) {
        {
            std::unique_lock lock(g_mutex);
            g_cv.wait(lock, [] { return g_press; });
            g_press = false;
        }
        if (g_on.load(std::memory_order_relaxed)) attempt();
        else if (g_watch.load(std::memory_order_relaxed)) {
            watch_line("Y");
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            watch_line("Y + 1 s");
        }
    }
}

}  // namespace

void init() {
    g_on = config::get_bool("Gestures", "ForceHogtie", false);
    const float r = config::get_float("Gestures", "ForceHogtieRange", 3.0f);
    g_range = !(r > 0.5f) ? 0.5f : r > 20.0f ? 20.0f : r;
    std::thread(worker).detach();
    g_watch = config::get_bool("Gestures", "HogtieWatch", false);
    std::thread(watcher).detach();
    log::info("[hogtie] Y hogties the one on the lasso (ForceHogtie) %d, within %.1f m", g_on.load() ? 1 : 0, g_range.load());
}

void on_game_y() {
    if (!g_on.load(std::memory_order_relaxed) && !g_watch.load(std::memory_order_relaxed)) return;
    {
        std::lock_guard lock(g_mutex);
        g_press = true;
    }
    g_cv.notify_one();
}

bool enabled() { return g_on.load(); }

void set_watch(bool on) {
    g_watch = on;
    log::info("[hogtie] HogtieWatch %d", on ? 1 : 0);
}

void set_force_session(bool on) {
    g_on = on;
    log::info("[hogtie] ForceHogtie %d (this session, not saved)", on ? 1 : 0);
}

void set_enabled(bool on) {
    if (g_on.exchange(on) != on) log::info("[hogtie] ForceHogtie %d", on ? 1 : 0);
    config::set("Gestures", "ForceHogtie", on ? "1" : "0");
}

}  // namespace rdrvr::hogtie
