// Adapted from gowj (Gears of War: Judgment ReXGlue port) src/gowj_fps_overlay.cpp (BSD-3-Clause, Copyright (c) 2026 the contributors to this project).
//
// cass - FPS / frame-time overlay.
//
// A separate click-through, topmost, no-activate layered window pinned to the game
// window's top-left corner and painted with GDI, so it works regardless of the
// runtime's presenter, any final-pass effect or ImGui (none of which it touches).
// Fed by the presenter hooks in cass_pacing.cpp: kind 0 = host present, kind 1 =
// guest output refresh (one per frame the game actually rendered); FPS is taken
// from the refresh stream.
//
// Shown only while the game window is the foreground window.
//   F9 toggles the overlay (cvar cass_fps_overlay, default off).
//   F8 toggles the frame limiter between off and the last non-zero cass_fps_limit
//      (60 if none was ever set).

#include "cass_fps_overlay.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

// Defined in cass_pacing.cpp.
REXCVAR_DECLARE(int32_t, cass_fps_limit);
REXCVAR_DECLARE(bool, cass_present_dedup);

REXCVAR_DEFINE_BOOL(cass_fps_overlay, false, "cass",
                    "Show the FPS / frame-time overlay in the top-left corner (F9 "
                    "toggles it in game; F8 toggles the cass_fps_limit frame limiter).");

namespace {

constexpr int kLines = 3;
constexpr int kLineH = 22;
constexpr int kOverlayW = 560;
constexpr int kOverlayH = kLines * kLineH + 8;

struct Sample {
  int64_t qpc;
  double ms;
};

std::mutex g_mu;
std::deque<Sample> g_refresh;  // last ~10 s of guest frames
std::deque<Sample> g_present;  // last ~2 s of presents
LARGE_INTEGER g_freq{};
HWND g_overlay = nullptr;
HWND g_game = nullptr;
char g_lines[kLines][110] = {"FPS --", "", ""};  // written and painted on the overlay thread

double QpcMs(int64_t q) { return 1000.0 * double(q) / double(g_freq.QuadPart); }

void Trim(std::deque<Sample>& d, int64_t now, double keep_ms) {
  while (!d.empty() && QpcMs(now - d.front().qpc) > keep_ms) d.pop_front();
}

HWND FindGameWindow() {
  struct Ctx {
    DWORD pid;
    HWND best;
    long best_area;
  } ctx{GetCurrentProcessId(), nullptr, 0};
  EnumWindows(
      [](HWND h, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<Ctx*>(lp);
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (pid != c->pid || !IsWindowVisible(h) || h == g_overlay) return TRUE;
        if (GetWindow(h, GW_OWNER)) return TRUE;
        RECT r{};
        GetClientRect(h, &r);
        const long area = (r.right - r.left) * (r.bottom - r.top);
        if (area > c->best_area) {
          c->best = h;
          c->best_area = area;
        }
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&ctx));
  return ctx.best;
}

void Recompute() {
  LARGE_INTEGER n;
  QueryPerformanceCounter(&n);
  int cnt = 0;
  double sum = 0, worst = 0, p99 = 0, pres_fps = 0;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    Trim(g_refresh, n.QuadPart, 10000.0);
    Trim(g_present, n.QuadPart, 2000.0);
    // Last 1 s of guest frames.
    for (auto it = g_refresh.rbegin(); it != g_refresh.rend(); ++it) {
      if (QpcMs(n.QuadPart - it->qpc) > 1000.0) break;
      ++cnt;
      sum += it->ms;
      worst = std::max(worst, it->ms);
    }
    if (cnt) {
      // 1% low over the last 10 s: fps equivalent of the 99th-percentile frame time.
      std::vector<double> all;
      all.reserve(g_refresh.size());
      for (const auto& s : g_refresh) all.push_back(s.ms);
      std::sort(all.begin(), all.end());
      p99 = all[std::min(all.size() - 1, size_t(double(all.size()) * 0.99))];
    }
    if (g_present.size() > 1) {
      pres_fps = 1000.0 * double(g_present.size() - 1) /
                 std::max(1.0, QpcMs(g_present.back().qpc - g_present.front().qpc));
    }
  }
  const int32_t lim = REXCVAR_GET(cass_fps_limit);
  char lim_txt[24];
  if (lim > 0) {
    snprintf(lim_txt, sizeof(lim_txt), "%d", static_cast<int>(lim));
  } else {
    snprintf(lim_txt, sizeof(lim_txt), "off");
  }
  if (cnt == 0) {
    snprintf(g_lines[0], sizeof(g_lines[0]), "FPS --");
    g_lines[1][0] = 0;
  } else {
    const double avg = sum / cnt;
    snprintf(g_lines[0], sizeof(g_lines[0]), "%.0f FPS  %.1f ms", 1000.0 / avg, avg);
    snprintf(g_lines[1], sizeof(g_lines[1]), "1%% low %.0f   worst %.0f ms",
             p99 > 0 ? 1000.0 / p99 : 0.0, worst);
  }
  snprintf(g_lines[2], sizeof(g_lines[2]), "present %.0f/s  dedup %s  limit %s [F8]", pres_fps,
           REXCVAR_GET(cass_present_dedup) ? "on" : "off", lim_txt);
}

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == WM_PAINT) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    RECT rc;
    GetClientRect(h, &rc);
    HBRUSH key = CreateSolidBrush(RGB(0, 0, 0));  // colour key = transparent
    FillRect(dc, &rc, key);
    DeleteObject(key);
    static HFONT font = CreateFontA(-20, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                                    NONANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");
    HGDIOBJ old = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    int y = 2;
    for (int i = 0; i < kLines; ++i) {
      if (!g_lines[i][0]) continue;
      const int len = int(strlen(g_lines[i]));
      SetTextColor(dc, RGB(24, 24, 24));  // shadow (not the key colour)
      TextOutA(dc, 3, y + 1, g_lines[i], len);
      SetTextColor(dc, i == 0 ? RGB(80, 255, 80) : RGB(230, 230, 230));
      TextOutA(dc, 2, y, g_lines[i], len);
      y += kLineH;
    }
    SelectObject(dc, old);
    EndPaint(h, &ps);
    return 0;
  }
  if (m == WM_NCHITTEST) return HTTRANSPARENT;
  return DefWindowProcA(h, m, w, l);
}

void OverlayThread() {
  WNDCLASSA wc{};
  wc.lpfnWndProc = WndProc;
  wc.hInstance = GetModuleHandleA(nullptr);
  wc.lpszClassName = "cass_fps_overlay";
  RegisterClassA(&wc);
  g_overlay = CreateWindowExA(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TRANSPARENT |
                                  WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                              wc.lpszClassName, "cass_fps", WS_POPUP, 0, 0, kOverlayW, kOverlayH,
                              nullptr, nullptr, wc.hInstance, nullptr);
  if (!g_overlay) {
    REXLOG_WARN("cass FPS overlay: window creation failed ({})", GetLastError());
    return;
  }
  SetLayeredWindowAttributes(g_overlay, RGB(0, 0, 0), 0, LWA_COLORKEY);
  REXLOG_INFO("cass FPS overlay ready (F9 toggles, F8 toggles the frame limiter)");
  bool f9_prev = false, f8_prev = false, shown = false;
  int tick = 0;
  int32_t saved_limit = 60;
  for (;;) {
    MSG msg;
    while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageA(&msg);
    }
    if (!g_game || !IsWindow(g_game)) g_game = FindGameWindow();
    const bool fg = g_game && GetForegroundWindow() == g_game && !IsIconic(g_game);
    const bool f9 = fg && (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    if (f9 && !f9_prev) {
      const bool next = !REXCVAR_GET(cass_fps_overlay);
      rex::cvar::SetFlagByName("cass_fps_overlay", next ? "true" : "false");
    }
    f9_prev = f9;
    const bool f8 = fg && (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    if (f8 && !f8_prev) {
      // cass_fps_limit is read by the pacer on every frame: 0 = off, N = cap.
      const int32_t cur = REXCVAR_GET(cass_fps_limit);
      if (cur > 0) saved_limit = cur;
      const int32_t next = cur > 0 ? 0 : saved_limit;
      const bool ok = rex::cvar::SetFlagByName("cass_fps_limit", std::to_string(next));
      REXLOG_INFO("cass frame limiter -> {} (was {}, applied={})", next, cur, ok);
    }
    f8_prev = f8;
    if (++tick % 5 != 0) {  // keys at 50 ms, window/text work at 250 ms
      Sleep(50);
      continue;
    }
    if (fg && REXCVAR_GET(cass_fps_overlay)) {
      POINT p{0, 0};
      ClientToScreen(g_game, &p);
      Recompute();
      SetWindowPos(g_overlay, HWND_TOPMOST, p.x + 12, p.y + 10, kOverlayW, kOverlayH,
                   SWP_NOACTIVATE | SWP_SHOWWINDOW);
      InvalidateRect(g_overlay, nullptr, TRUE);
      shown = true;
    } else if (shown) {
      ShowWindow(g_overlay, SW_HIDE);
      shown = false;
    }
    Sleep(50);
  }
}

}  // namespace

namespace rex::glue {

void FpsOverlayNote(size_t kind, uint64_t interval_us) {
  if (!interval_us || kind > 1 || !g_freq.QuadPart) return;
  LARGE_INTEGER n;
  QueryPerformanceCounter(&n);
  std::lock_guard<std::mutex> lk(g_mu);
  auto& d = kind == 1 ? g_refresh : g_present;
  d.push_back({n.QuadPart, double(interval_us) / 1000.0});
  // Bounded even while the overlay is hidden (Recompute only runs when shown).
  Trim(d, n.QuadPart, kind == 1 ? 10000.0 : 2000.0);
}

void InstallFpsOverlay() {
  static bool done = false;
  if (done) return;
  done = true;
  QueryPerformanceFrequency(&g_freq);
  std::thread(OverlayThread).detach();
}

}  // namespace rex::glue
