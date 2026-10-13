#include "core/eye_capture.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>

#include "core/config.h"
#include "core/eye_grab.h"
#include "core/log.h"

namespace rdrvr::eye_capture {
namespace {

std::mutex g_mutex;  // g_dir, the capture itself (one at a time)
std::string g_dir;
std::atomic<int> g_period_s{0};  // 0: off
std::atomic<bool> g_thread{false};
std::atomic<uint64_t> g_taken{0}, g_failed{0};
std::string g_last = "none yet";
std::atomic<bool> g_async{false};

std::string default_dir() {
    wchar_t w[MAX_PATH];
    log::path_in_game_dir(L"RDRVR_eyecap", w, MAX_PATH);
    char a[MAX_PATH * 2];
    WideCharToMultiByte(CP_UTF8, 0, w, -1, a, sizeof(a), nullptr, nullptr);
    return a;
}

// One pair now: <dir>\eyes_<yyyymmdd-hhmmss>_<n>-L/-R.rde
std::string take(const std::string& dir) {
    std::lock_guard lock(g_mutex);
    CreateDirectoryA(dir.c_str(), nullptr);
    SYSTEMTIME st;
    GetLocalTime(&st);
    char prefix[MAX_PATH * 2];
    std::snprintf(prefix, sizeof(prefix), "%s\\eyes_%04u%02u%02u-%02u%02u%02u_%llu", dir.c_str(), st.wYear, st.wMonth, st.wDay, st.wHour,
                  st.wMinute, st.wSecond, static_cast<unsigned long long>(g_taken.load() + g_failed.load()));
    const std::string r = eye_grab::grab(prefix);
    const bool both = r.find("NOT MEASURED") == std::string::npos && r.find("ERROR") == std::string::npos;
    (both ? g_taken : g_failed).fetch_add(1, std::memory_order_relaxed);
    g_last = r;
    log::info("[eyecap] %s", r.c_str());
    return r;
}

void loop() {
    for (;;) {
        const int p = g_period_s.load();
        if (p <= 0) {
            g_thread = false;
            return;
        }
        std::string dir;
        {
            std::lock_guard lock(g_mutex);
            dir = g_dir;
        }
        take(dir);
        for (int i = 0; i < p * 10 && g_period_s.load() > 0; ++i) Sleep(100);
    }
}

void start(int seconds, const std::string& dir) {
    {
        std::lock_guard lock(g_mutex);
        g_dir = dir.empty() ? (g_dir.empty() ? default_dir() : g_dir) : dir;
    }
    g_period_s = seconds;
    if (seconds > 0 && !g_thread.exchange(true)) std::thread(loop).detach();
    log::info("[eyecap] every %d s into %s", seconds, g_dir.c_str());
}

}  // namespace

void init() {
    const int s = config::get_int("Debug", "EyeCapture", 0);
    if (s > 0) start(s, config::get_string("Debug", "EyeCaptureDir", ""));
}

bool take_async(void (*done)(bool ok)) {
    if (g_async.exchange(true)) return false;
    std::thread([done] {
        std::string dir;
        {
            std::lock_guard lock(g_mutex);
            dir = g_dir.empty() ? default_dir() : g_dir;
        }
        const std::string r = take(dir);
        const bool ok = r.find("NOT MEASURED") == std::string::npos && r.find("ERROR") == std::string::npos;
        if (done) done(ok);
        g_async = false;
    }).detach();
    return true;
}

std::string command(const std::vector<std::string>& t) {
    const std::string sub = t.size() >= 2 ? t[1] : "status";
    if (sub == "on") {
        start(t.size() >= 3 ? std::atoi(t[2].c_str()) : 10, t.size() >= 4 ? t[3] : "");
        return "eyecap every " + std::to_string(g_period_s.load()) + " s into " + g_dir;
    }
    if (sub == "off") {
        g_period_s = 0;
        return "eyecap off";
    }
    if (sub == "now") {
        std::string dir;
        {
            std::lock_guard lock(g_mutex);
            dir = t.size() >= 3 ? t[2] : (g_dir.empty() ? default_dir() : g_dir);
        }
        return take(dir);
    }
    std::lock_guard lock(g_mutex);
    return "eyecap " + (g_period_s.load() > 0 ? "every " + std::to_string(g_period_s.load()) + " s" : std::string("off")) + ", " +
           std::to_string(g_taken.load()) + " pairs, " + std::to_string(g_failed.load()) + " incomplete; last: " + g_last;
}

}  // namespace rdrvr::eye_capture
