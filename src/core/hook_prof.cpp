#include "core/hook_prof.h"

#include <windows.h>

#include <cstdio>

namespace rdrvr::hookprof {
namespace {
double g_t0 = 0;  // the window's start (ms)
double now_ms() {
    LARGE_INTEGER q, f;
    QueryPerformanceCounter(&q);
    QueryPerformanceFrequency(&f);
    return 1000.0 * static_cast<double>(q.QuadPart) / static_cast<double>(f.QuadPart);
}
double tsc_per_ms() {  // (measured once over 50 ms)
    static const double r = [] {
        const double a = now_ms();
        const uint64_t c = __rdtsc();
        Sleep(50);
        return static_cast<double>(__rdtsc() - c) / (now_ms() - a);
    }();
    return r;
}
}  // namespace

std::string command(const std::string& sub) {
    if (sub == "on" || sub == "reset") {
        tsc_per_ms();
        for (Acc& a : g_acc) a.calls = 0, a.total = 0, a.orig = 0;
        g_t0 = now_ms();
        g_on = true;
        return "hookprof on (counts reset)";
    }
    if (sub == "off") {
        g_on = false;
        return "hookprof off";
    }
    // the window's totals as ms per second of game time and ns per call: the hook's own work, then D3D12's
    const double secs = (now_ms() - g_t0) / 1000.0, k = tsc_per_ms();
    if (secs <= 0 || k <= 0) return "hookprof: nothing measured (hookprof on first)";
    std::string out;
    char b[200];
    double own_sum = 0, d3d_sum = 0;
    for (int i = 0; i < kCount; ++i) {
        const double n = static_cast<double>(g_acc[i].calls.load()), tot = static_cast<double>(g_acc[i].total.load()),
                     orig = static_cast<double>(g_acc[i].orig.load());
        if (n == 0) continue;
        own_sum += (tot - orig) / k / secs, d3d_sum += orig / k / secs;
        std::snprintf(b, sizeof(b), "%s%s %.0f/s: own %.2f ms/s (%.0f ns), d3d %.2f ms/s (%.0f ns)", out.empty() ? "" : " | ", kNames[i],
                      n / secs, (tot - orig) / k / secs, (tot - orig) / k * 1e6 / n, orig / k / secs, orig / k * 1e6 / n);
        out += b;
    }
    std::snprintf(b, sizeof(b), " || %.1f s: own %.2f ms/s, d3d %.2f ms/s", secs, own_sum, d3d_sum);
    return out + b;
}

}  // namespace rdrvr::hookprof
