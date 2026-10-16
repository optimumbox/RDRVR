#include "core/hogtie.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <initializer_list>
#include <mutex>
#include <thread>

#include "common/rdrvr_api.h"
#include "core/api.h"
#include "core/config.h"
#include "core/log.h"

namespace rdrvr::hogtie {
namespace {

// The natives (MagicRDR's natives.json; each hash checked against its name's joaat)
constexpr uint32_t kGetLassoTarget = 0xAA364907u;     // GET_LASSO_TARGET(user) -> the roped actor
constexpr uint32_t kCanHogtie = 0x1AA3A0C0u;          // CAN_ACTOR_HOGTIE_TARGET(actor, target) (logged only)
constexpr uint32_t kIsHogtied = 0xA610DC79u;          // IS_ACTOR_HOGTIED(actor)
constexpr uint32_t kHogtieActor = 0x4440BCA5u;        // HOGTIE_ACTOR(actor)
constexpr uint32_t kGetPosition = 0x99BD9D6Fu;        // GET_POSITION(actor, Vector3*)
constexpr uint32_t kWaitMs = 500;                     // a native's result: the next script tick or two
constexpr int kGameFirstMs = 400;                     // the game's own hogtie given that long to start

std::atomic<bool> g_on{true};
std::atomic<float> g_range{3.0f};  // [Gestures] ForceHogtieRange (m)
std::mutex g_mutex;
std::condition_variable g_cv;
bool g_press = false;  // under g_mutex
std::atomic<uint64_t> g_forced{0};

bool call(uint32_t hash, std::initializer_list<uint64_t> a, RdrvrNativeResult* r, uint32_t vec_out = 0) {
    uint64_t args[4] = {};
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
    }
}

}  // namespace

void init() {
    g_on = config::get_bool("Gestures", "ForceHogtie", true);
    const float r = config::get_float("Gestures", "ForceHogtieRange", 3.0f);
    g_range = !(r > 0.5f) ? 0.5f : r > 20.0f ? 20.0f : r;
    std::thread(worker).detach();
    log::info("[hogtie] Y hogties the one on the lasso (ForceHogtie) %d, within %.1f m", g_on.load() ? 1 : 0, g_range.load());
}

void on_game_y() {
    if (!g_on.load(std::memory_order_relaxed)) return;
    {
        std::lock_guard lock(g_mutex);
        g_press = true;
    }
    g_cv.notify_one();
}

bool enabled() { return g_on.load(); }

void set_enabled(bool on) {
    if (g_on.exchange(on) != on) log::info("[hogtie] ForceHogtie %d", on ? 1 : 0);
    config::set("Gestures", "ForceHogtie", on ? "1" : "0");
}

}  // namespace rdrvr::hogtie
