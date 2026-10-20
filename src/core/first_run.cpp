#include "core/first_run.h"

#include <intrin.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <cwctype>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/hooks.h"
#include "core/log.h"

namespace rdrvr::first_run {
namespace {

constexpr int kReadSeconds = 30;
constexpr wchar_t kSentence[] = L"I have fully read this and understand.";

DWORD g_main_tid = 0;
std::atomic<int> g_state{0};  // 0 not armed (off, or read before), 1 to be shown, 2 showing, 3 read

using CreateW_t = HWND(WINAPI*)(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
using CreateA_t = HWND(WINAPI*)(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
CreateW_t o_create_w = nullptr;
CreateA_t o_create_a = nullptr;

std::wstring marker_path() {
    std::wstring p = config::user_ini_path();
    const size_t slash = p.find_last_of(L'\\');
    return slash == std::wstring::npos ? std::wstring() : p.substr(0, slash) + L"\\FirstRunNotice.read";
}

bool from_game(const void* ret) {
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const auto r = reinterpret_cast<uintptr_t>(ret);
    return r >= base && r < base + nt->OptionalHeader.SizeOfImage;
}

// ---- the window ----

enum : int { kIdShowLog = 101, kIdTyped, kIdContinue, kIdQuit, kTimer = 1 };

constexpr COLORREF kInk = RGB(31, 31, 31), kGrey = RGB(90, 90, 90), kRed = RGB(196, 43, 28), kGreen = RGB(16, 124, 16),
                   kAmber = RGB(157, 93, 0);

struct Ui {
    HWND wnd = nullptr, typed = nullptr, timer = nullptr, cont = nullptr;
    HFONT title = nullptr, body = nullptr, bold = nullptr, big = nullptr, mono = nullptr;
    HBRUSH white = nullptr;
    std::vector<std::pair<HWND, COLORREF>> colors;
    std::wstring log_path, game_dir;
    ULONGLONG start = 0;
    bool done = false;
};
Ui* g_ui = nullptr;

int seconds_left() {
    const ULONGLONG ms = GetTickCount64() - g_ui->start;
    const int left = kReadSeconds - static_cast<int>(ms / 1000);
    return left < 0 ? 0 : left;
}

// what was typed, as the sentence: case, spaces and the end's full stop not counted
std::wstring normal(const std::wstring& s) {
    std::wstring o;
    for (wchar_t c : s) {
        if (std::iswspace(c)) {
            if (!o.empty() && o.back() != L' ') o += L' ';
        } else {
            o += static_cast<wchar_t>(std::towlower(c));
        }
    }
    while (!o.empty() && (o.back() == L' ' || o.back() == L'.')) o.pop_back();
    return o;
}

bool typed_ok() {
    wchar_t buf[256] = {};
    GetWindowTextW(g_ui->typed, buf, 255);
    return normal(buf) == normal(kSentence);
}

void set_color(HWND h, COLORREF c) {
    for (auto& [w, col] : g_ui->colors)
        if (w == h) {
            col = c;
            InvalidateRect(h, nullptr, TRUE);
            return;
        }
    g_ui->colors.emplace_back(h, c);
}

void refresh() {
    const int left = seconds_left();
    const bool ok = typed_ok();
    wchar_t text[160];
    if (left > 0) {
        swprintf_s(text, L"Please keep reading. Continue unlocks in %d second%s.", left, left == 1 ? L"" : L"s");
        set_color(g_ui->timer, kAmber);
    } else if (!ok) {
        swprintf_s(text, L"Time is up. Now type the sentence above in the box to continue.");
        set_color(g_ui->timer, kRed);
    } else {
        swprintf_s(text, L"Thank you. Click Continue to start the game.");
        set_color(g_ui->timer, kGreen);
    }
    wchar_t old[160] = {};
    GetWindowTextW(g_ui->timer, old, 159);
    if (wcscmp(old, text)) SetWindowTextW(g_ui->timer, text);
    EnableWindow(g_ui->cont, left == 0 && ok);
}

void quit_game(const char* why) {
    if (MessageBoxW(g_ui->wnd, L"Quit now? The game will not start.\n\nYou will see this message again next time.", L"RDRVR",
                    MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
        return;
    log::info("[firstrun] the notice closed (%s): the game quit", why);
    TerminateProcess(GetCurrentProcess(), 0);
}

LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wp);
            COLORREF c = kInk;
            for (auto& [w, col] : g_ui->colors)
                if (w == reinterpret_cast<HWND>(lp)) c = col;
            SetTextColor(dc, c);
            SetBkColor(dc, RGB(255, 255, 255));
            return reinterpret_cast<LRESULT>(g_ui->white);
        }
        case WM_TIMER:
            refresh();
            return 0;
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case kIdShowLog: {
                    const std::wstring args = L"/select,\"" + g_ui->log_path + L"\"";
                    ShellExecuteW(h, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
                    log::info("[firstrun] \"Show me my RDRVR.log\" clicked");
                    return 0;
                }
                case kIdTyped:
                    if (HIWORD(wp) == EN_CHANGE) refresh();
                    return 0;
                case kIdContinue:
                    if (seconds_left() == 0 && typed_ok()) g_ui->done = true;
                    return 0;
                case kIdQuit:
                    quit_game("Quit");
                    return 0;
            }
            break;
        case WM_CLOSE:
            quit_game("the window's X");
            return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

// Places the window's controls top to bottom at scale s (pixels per 96-dpi pixel). With create false it only measures;
// returns the client height.
int layout(float s, bool create, HINSTANCE inst) {
    auto S = [s](int v) { return static_cast<int>(v * s + 0.5f); };
    Ui& u = *g_ui;
    const int W = S(720), margin = S(30), cw = W - 2 * margin;
    for (HFONT f : {u.title, u.body, u.bold, u.big, u.mono})
        if (f) DeleteObject(f);
    auto font = [&](int px, int weight, const wchar_t* face) {
        return CreateFontW(-S(px), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH, face);
    };
    u.title = font(27, FW_BOLD, L"Segoe UI");
    u.body = font(16, FW_NORMAL, L"Segoe UI");
    u.bold = font(16, FW_BOLD, L"Segoe UI");
    u.big = font(19, FW_BOLD, L"Segoe UI");
    u.mono = font(15, FW_NORMAL, L"Consolas");

    HDC dc = GetDC(nullptr);
    int y = S(22);
    auto measure = [&](const std::wstring& t, HFONT f, int width) {
        RECT r{0, 0, width, 0};
        HGDIOBJ old = SelectObject(dc, f);
        DrawTextW(dc, t.c_str(), -1, &r, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX | DT_EXPANDTABS);
        SelectObject(dc, old);
        return static_cast<int>(r.bottom);
    };
    auto control = [&](const wchar_t* cls, const std::wstring& t, DWORD style, int x, int yy, int w, int hh, HFONT f, int id,
                       COLORREF c, DWORD ex = 0) -> HWND {
        if (!create) return nullptr;
        HWND c_ = CreateWindowExW(ex, cls, t.c_str(), WS_CHILD | WS_VISIBLE | style, x, yy, w, hh, u.wnd,
                                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst, nullptr);
        SendMessageW(c_, WM_SETFONT, reinterpret_cast<WPARAM>(f), FALSE);
        if (!std::wcscmp(cls, L"STATIC") || (style & ES_READONLY)) u.colors.emplace_back(c_, c);
        return c_;
    };
    auto text = [&](const std::wstring& t, HFONT f, COLORREF c, int gap_after, int indent = 0) {
        const int hh = measure(t, f, cw - indent);
        control(L"STATIC", t, SS_LEFT | SS_NOPREFIX, margin + indent, y, cw - indent, hh, f, -1, c);
        y += hh + S(gap_after);
    };
    auto step = [&](int n, const std::wstring& t, int gap_after) {
        const int num_w = S(30);
        const int hh = measure(t, u.body, cw - num_w);
        control(L"STATIC", std::to_wstring(n) + L".", SS_LEFT, margin, y, num_w, hh, u.bold, -1, kInk);
        control(L"STATIC", t, SS_LEFT | SS_NOPREFIX, margin + num_w, y, cw - num_w, hh, u.body, -1, kInk);
        y += hh + S(gap_after);
    };

    text(L"Welcome to RDRVR! Please read this.", u.title, kInk, 6);
    text(L"You only see this once. The game starts after you finish here.", u.body, kGrey, 16);
    text(L"Need help? Every help post MUST include your RDRVR.log file. Without it, nobody can tell what went wrong, "
         L"so posts without it can't be helped.",
         u.bold, kRed, 10);
    text(L"RDRVR.log is a file the mod writes while you play. It records what happened, so it shows us the problem.", u.body, kInk, 18);
    text(L"How to send RDRVR.log in a Discord chat", u.big, kInk, 10);
    step(1, L"Quit the game first. Don't start it again until you have sent the log: each start makes a new RDRVR.log.", 6);
    step(2, L"Open your game folder. On Steam: in your Library, right-click Red Dead Redemption, then click Manage, then "
            L"Browse local files. Or open this folder:",
         6);
    {
        const int hh = S(26), indent = S(30);
        control(L"EDIT", u.game_dir, ES_READONLY | ES_AUTOHSCROLL, margin + indent, y, cw - indent, hh, u.mono, -1, kInk,
                WS_EX_CLIENTEDGE);
        y += hh + S(8);
    }
    step(3, L"Find the file called RDRVR.log. If your names have no \".log\" at the end, it is the \"RDRVR\" whose Type says "
            L"\"Text Document\".",
         6);
    step(4, L"Click and hold on the file, drag it into the Discord chat, and let go. (Or right-click the file, click Copy, "
            L"click in Discord's message box and press Ctrl+V.)",
         6);
    step(5, L"Type what went wrong, then press Enter.", 10);
    text(L"Discord says the file is too big? Right-click RDRVR.log, click \"Send to\", then \"Compressed (zipped) folder\". "
         L"Send the new .zip file instead.",
         u.body, kGrey, 6);
    text(L"Send the file itself. Not a photo or a screenshot of it.", u.bold, kInk, 14);
    {
        const int bw = S(230), bh = S(36);
        control(L"BUTTON", L"Show me my RDRVR.log", BS_PUSHBUTTON | WS_TABSTOP, margin, y, bw, bh, u.bold, kIdShowLog, kInk);
        const std::wstring t = L"Try it now: opens the folder with RDRVR.log picked out.";
        const int tx = margin + bw + S(14), th = measure(t, u.body, cw - bw - S(14));
        control(L"STATIC", t, SS_LEFT | SS_NOPREFIX, tx, y + (bh - th) / 2, cw - bw - S(14), th, u.body, -1, kGrey);
        y += bh + S(18);
    }
    control(L"STATIC", L"", SS_ETCHEDHORZ, margin, y, cw, 2, u.body, -1, kInk);
    y += S(16);
    text(L"To continue, type this sentence in the box below:", u.body, kInk, 4);
    text(kSentence, u.big, kInk, 8);
    {
        const int hh = S(32);
        u.typed = control(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, margin, y, cw, hh, u.body, kIdTyped, kInk, WS_EX_CLIENTEDGE);
        y += hh + S(10);
    }
    {
        const int hh = measure(L"Xg", u.bold, cw);
        u.timer = control(L"STATIC", L"", SS_LEFT | SS_NOPREFIX, margin, y, cw, hh, u.bold, -1, kAmber);
        y += hh + S(14);
    }
    {
        const int bw = S(200), qw = S(100), bh = S(40);
        control(L"BUTTON", L"Quit", BS_PUSHBUTTON | WS_TABSTOP, margin + cw - bw - S(12) - qw, y, qw, bh, u.body, kIdQuit, kInk);
        u.cont = control(L"BUTTON", L"Continue to the game", BS_DEFPUSHBUTTON | WS_TABSTOP | WS_DISABLED, margin + cw - bw, y, bw, bh,
                         u.bold, kIdContinue, kInk);
        y += bh + S(22);
    }
    ReleaseDC(nullptr, dc);
    return y;
}

void show(const char* where) {
    Ui u;
    g_ui = &u;
    wchar_t path[MAX_PATH];
    log::path_in_game_dir(L"RDRVR.log", path, MAX_PATH);
    u.log_path = path;
    u.game_dir = u.log_path.substr(0, u.log_path.find_last_of(L'\\'));
    log::info("[firstrun] the first-run notice shown (from %s on thread %lu)", where, GetCurrentThreadId());

    const DPI_AWARENESS_CONTEXT old_dpi = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HINSTANCE inst = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&show), &inst);
    u.white = CreateSolidBrush(RGB(255, 255, 255));
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = u.white;
    wc.lpszClassName = L"RDRVR_FirstRunNotice";
    wc.hIcon = LoadIconW(nullptr, IDI_INFORMATION);
    RegisterClassExW(&wc);

    // the scale: the screen's, smaller if the window would not fit its height
    const UINT dpi = GetDpiForSystem();
    RECT work{0, 0, 1920, 1040};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    float s = dpi / 96.0f;
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, ex = WS_EX_CONTROLPARENT | WS_EX_APPWINDOW;
    auto outer = [&](int cw_, int ch_) {
        RECT r{0, 0, cw_, ch_};
        AdjustWindowRectExForDpi(&r, style, FALSE, ex, dpi);
        return r;
    };
    int ch = layout(s, false, inst);
    RECT r = outer(static_cast<int>(720 * s + 0.5f), ch);
    const int avail = work.bottom - work.top;
    if (r.bottom - r.top > avail) {
        s *= static_cast<float>(avail) / static_cast<float>(r.bottom - r.top) * 0.98f;
        ch = layout(s, false, inst);
        r = outer(static_cast<int>(720 * s + 0.5f), ch);
    }
    const int ww = r.right - r.left, wh = r.bottom - r.top;
    const int x = work.left + ((work.right - work.left) - ww) / 2, y = work.top + std::max(0, (avail - wh) / 2);
    u.wnd = CreateWindowExW(ex, wc.lpszClassName, L"RDRVR - read this before you play", style, x, y, ww, wh, nullptr, nullptr, inst, nullptr);
    if (!u.wnd) {
        log::error("[firstrun] the notice's window not made (error %lu): the game goes on without it", GetLastError());
        SetThreadDpiAwarenessContext(old_dpi);
        g_ui = nullptr;
        return;
    }
    layout(s, true, inst);
    u.start = GetTickCount64();
    refresh();
    SetTimer(u.wnd, kTimer, 250, nullptr);
    ShowWindow(u.wnd, SW_SHOWNORMAL);
    // brought to the front (above the launcher), then left an ordinary window so the folder it opens can come over it
    SetWindowPos(u.wnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    SetWindowPos(u.wnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    SetForegroundWindow(u.wnd);
    FLASHWINFO fl{sizeof(fl), u.wnd, FLASHW_ALL | FLASHW_TIMERNOFG, 0, 0};
    FlashWindowEx(&fl);
    SetFocus(u.typed);

    MSG msg;
    while (!u.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN && msg.hwnd == u.typed) {
            SendMessageW(u.wnd, WM_COMMAND, kIdContinue, 0);
            continue;
        }
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) continue;  // never a quit by a stray key
        if (IsDialogMessageW(u.wnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    KillTimer(u.wnd, kTimer);
    DestroyWindow(u.wnd);
    UnregisterClassW(wc.lpszClassName, inst);
    for (HFONT f : {u.title, u.body, u.bold, u.big, u.mono}) DeleteObject(f);
    DeleteObject(u.white);
    SetThreadDpiAwarenessContext(old_dpi);
    g_ui = nullptr;

    const std::wstring marker = marker_path();
    bool saved = false;
    if (!marker.empty()) {
        CreateDirectoryW(marker.substr(0, marker.find_last_of(L'\\')).c_str(), nullptr);
        HANDLE f = CreateFileW(marker.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            static const char kText[] = "The RDRVR first-run notice was read. Delete this file to see it again.\r\n";
            DWORD w = 0;
            saved = WriteFile(f, kText, sizeof(kText) - 1, &w, nullptr) != 0;
            CloseHandle(f);
        }
    }
    log::info("[firstrun] the notice read: the game goes on (%ls %s)", marker.c_str(), saved ? "written" : "NOT WRITTEN: shown again next time");
}

HWND WINAPI hk_create_w(DWORD ex, LPCWSTR cls, LPCWSTR name, DWORD style, int x, int y, int w, int h, HWND parent, HMENU menu,
                        HINSTANCE inst, LPVOID param) {
    if (g_state.load(std::memory_order_relaxed) == 1 && (GetCurrentThreadId() == g_main_tid || from_game(_ReturnAddress())))
        gate("CreateWindowExW");
    return o_create_w(ex, cls, name, style, x, y, w, h, parent, menu, inst, param);
}

HWND WINAPI hk_create_a(DWORD ex, LPCSTR cls, LPCSTR name, DWORD style, int x, int y, int w, int h, HWND parent, HMENU menu,
                        HINSTANCE inst, LPVOID param) {
    if (g_state.load(std::memory_order_relaxed) == 1 && (GetCurrentThreadId() == g_main_tid || from_game(_ReturnAddress())))
        gate("CreateWindowExA");
    return o_create_a(ex, cls, name, style, x, y, w, h, parent, menu, inst, param);
}

}  // namespace

void on_process_attach() { g_main_tid = GetCurrentThreadId(); }

void install() {
    if (!config::get_bool("Launcher", "FirstRunNotice", true)) return;
    const std::wstring marker = marker_path();
    if (marker.empty() || GetFileAttributesW(marker.c_str()) != INVALID_FILE_ATTRIBUTES) {
        log::info("[firstrun] the first-run notice was read before (%ls): not shown", marker.c_str());
        return;
    }
    HMODULE user32 = LoadLibraryW(L"user32.dll");
    const bool ok = user32 &&
                    hooks::install("user32!CreateWindowExW", reinterpret_cast<void*>(GetProcAddress(user32, "CreateWindowExW")), hk_create_w,
                                   &o_create_w) &&
                    hooks::install("user32!CreateWindowExA", reinterpret_cast<void*>(GetProcAddress(user32, "CreateWindowExA")), hk_create_a,
                                   &o_create_a);
    g_state = 1;  // (also D3D12CreateDevice's gate, if the hooks did not go in)
    log::info("[firstrun] the first-run notice is to be shown before the game's window (main thread %lu; window hooks %s)", g_main_tid,
              ok ? "in" : "NOT in: shown at the device instead");
}

void gate(const char* where) {
    // the device's gate on the game's main thread only (CreateWindowEx's hooks also take the game's own calls elsewhere)
    if (!std::strcmp(where, "D3D12CreateDevice") && GetCurrentThreadId() != g_main_tid) return;
    int expect = 1;
    if (!g_state.compare_exchange_strong(expect, 2)) return;
    show(where);
    g_state = 3;
}

}  // namespace rdrvr::first_run
