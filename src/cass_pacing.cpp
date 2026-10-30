// Adapted from gowj (Gears of War: Judgment ReXGlue port) src/gowj_perf_watch.cpp (BSD-3-Clause, Copyright (c) 2026 the contributors to this project).
//
// cass - host frame pacing.
//
// Two interventions on the runtime's D3D12 presenter, both live cvars (read per
// call, so the config file, the command line and runtime changes all apply):
//
//   cass_present_dedup (bool, true) - drop a host swap commit when no new guest
//     frame exists. The stock presenter re-commits the same image several times
//     per guest frame (Judgment measured 139-220 swaps/s against 51-59 guest
//     frames/s), each one a full-resolution pass through the gamma ramp, the
//     post effect and the dither. Skipping those handed the GPU time back to the
//     guest renderer: Judgment went 51.3 -> 61.4 fps and p99 71.5 -> 30.8 ms. A
//     33 ms floor still repaints a stale image so UI that animates without the
//     guest producing frames keeps moving at >= 30 Hz.
//
//   cass_fps_limit (int, 0 = off) - release the guest render thread on a fixed
//     QPC grid at N Hz: a high-resolution waitable timer sleeps to just short of
//     the slot, then a short spin lands it. A frame that already missed its slot
//     is released at once and skips it, so an over-budget scene keeps its real
//     rate instead of collapsing onto every second grid line. Waiting on the
//     guest's render thread (not the UI thread) is what a natively capped title
//     does: the guest stops queueing GPU work while it waits.
//
// Both entry points are virtual overrides of rex::ui::Presenter, reached through
// the class vtable that rexruntime.dll exports as a data symbol
// (??_7D3D12Presenter@d3d12@ui@rex@@6B@). The hook rewrites one pointer slot per
// function; the slot is found by value (the exported function address), so a
// runtime rebuild that reorders the class logs "not found" instead of hooking the
// wrong function. Verified against the stock rexruntime.dll (SDK 923c1a5):
// RefreshGuestOutputImpl is vtable slot 5, PaintAndPresentImpl slot 6. If an
// earlier hook in this executable (cass_fsr.cpp) already owns slot 5, this one
// chains onto it at that verified index; so call InstallFsr() before this.
//
// The Judgment port's measurement/CSV/D3DKMT-probe/scanout-phase machinery is not
// carried over; only what the two interventions and the FPS overlay need is.

#include "cass_pacing.h"

#include "cass_fps_overlay.h"
#include "cass_system_tuning.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <new>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/d3d12/d3d12_presenter.h>
#include <rex/ui/presenter.h>

REXCVAR_DEFINE_BOOL(cass_present_dedup, true, "cass",
                    "Skip a host swap commit when no new guest frame exists (a 33 ms "
                    "floor still repaints a stale image). The runtime otherwise "
                    "re-commits the same image several times per guest frame.");

REXCVAR_DEFINE_INT32(cass_fps_limit, 0, "cass",
                     "Frame limiter in frames per second (0 = off). Releases the guest "
                     "render thread on a fixed high-resolution timer grid. For a VRR "
                     "(G-Sync/FreeSync) display use a few fps under the refresh rate, "
                     "e.g. 141 on 144 Hz.");

REXCVAR_DEFINE_INT32(cass_fps_limit_spin_us, 1500, "cass",
                     "Frame limiter: microseconds before each slot to stop sleeping and "
                     "spin. Larger = more accurate release, more CPU. 0 = sleep only.");

namespace {

constexpr size_t kVtableScanSlots = 96;
// Repaint ceiling for a stale guest image, so UI that animates without the guest
// producing a frame (menu fades, a modal that pauses guest output) still moves.
constexpr uint64_t kUiFloorUs = 33333;

LARGE_INTEGER g_qpc_freq{};

uint64_t TicksToUs(int64_t ticks) {
  const int64_t f = g_qpc_freq.QuadPart;
  if (f <= 0 || ticks <= 0) {
    return 0;
  }
  // Split so a long uptime cannot overflow ticks * 1e6.
  return static_cast<uint64_t>((ticks / f) * 1000000 + (ticks % f) * 1000000 / f);
}

uint64_t NowUs() {
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  return TicksToUs(now.QuadPart);
}

// Inter-event interval per series, for the FPS overlay. PaintAndPresentImpl may
// run on the UI thread or (immediate-paint mode) on the guest output thread, so
// the last timestamp is atomic rather than owned by one thread.
struct Interval {
  std::atomic<int64_t> last{0};
  uint64_t Tick() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const int64_t prev = last.exchange(now.QuadPart, std::memory_order_relaxed);
    return prev ? TicksToUs(now.QuadPart - prev) : 0;
  }
};
Interval g_present_iv;
Interval g_refresh_iv;

std::atomic<uint64_t> g_guest_seq{0};
std::atomic<uint64_t> g_shown_seq{0};
std::atomic<uint64_t> g_skipped{0};
std::atomic<int64_t> g_last_present{0};  // QPC ticks of the last real present

std::atomic<rex::glue::GuestOutputObserver> g_observer{nullptr};

// ---------------------------------------------------------------------------
// Redundant-present suppression.
//
// The gate is the guest frame counter, not the caller's execute_ui_drawers flag:
// Judgment measured that argument true on 100% of ~44 000 presents, because the
// runtime composites its UI pass every tick whether or not anything is overlaid.

// Presenter::PaintResult is a protected enum class (underlying int); returned in
// eax, so it is carried here as its integer value. 0 = kPresented.
using PaintResult = int32_t;
constexpr PaintResult kPaintPresented = 0;
constexpr PaintResult kPaintPresentedSuboptimal = 1;
// PaintResult D3D12Presenter::PaintAndPresentImpl(bool execute_ui_drawers)
using PresentFn = PaintResult (*)(void* self, bool execute_ui_drawers);
PresentFn g_present_orig = nullptr;

PaintResult HookPresent(void* self, bool execute_ui_drawers) {
  LARGE_INTEGER owned;
  QueryPerformanceCounter(&owned);
  const uint64_t guest = g_guest_seq.load(std::memory_order_acquire);
  const bool fresh = guest != g_shown_seq.load(std::memory_order_relaxed);
  const int64_t last = g_last_present.load(std::memory_order_relaxed);
  if (!fresh && REXCVAR_GET(cass_present_dedup) && last != 0 &&
      TicksToUs(owned.QuadPart - last) < kUiFloorUs) {
    // Nothing new to show: the previous commit is still on screen.
    const uint64_t n = g_skipped.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n == 1) {
      REXLOG_INFO("cass pacing: redundant-present suppression is active");
    }
    return kPaintPresented;
  }
  const PaintResult result = g_present_orig(self, execute_ui_drawers);
  // Only count the frame as on screen if it really reached the OS (kPresented or
  // kPresentedSuboptimal); after a refused/outdated paint the next call retries.
  if (result == kPaintPresented || result == kPaintPresentedSuboptimal) {
    LARGE_INTEGER done;
    QueryPerformanceCounter(&done);
    g_last_present.store(done.QuadPart, std::memory_order_relaxed);
    g_shown_seq.store(guest, std::memory_order_relaxed);
    rex::glue::FpsOverlayNote(0, g_present_iv.Tick());
  }
  return result;
}

// ---------------------------------------------------------------------------
// Frame limiter on a QPC grid (cass_fps_limit). Runs on the guest render thread,
// after a successful guest-output refresh. Only one thread owns the grid; any
// other caller passes straight through.

HANDLE g_pace_timer = nullptr;
std::atomic<uint32_t> g_pace_owner{0};
uint64_t g_pace_interval_us = 0;  // owner thread only
uint64_t g_pace_anchor = 0;       // grid origin, owner thread only
uint64_t g_pace_due = 0;          // next release time, owner thread only

void PaceToGrid() {
  const uint32_t tid = GetCurrentThreadId();
  uint32_t owner = g_pace_owner.load(std::memory_order_relaxed);
  if (owner == 0) {
    g_pace_owner.compare_exchange_strong(owner, tid, std::memory_order_relaxed);
    owner = g_pace_owner.load(std::memory_order_relaxed);
  }
  if (owner != tid) {
    return;
  }

  // Read per call; re-anchor whenever the cadence changes or the limiter is off.
  const int32_t fps = REXCVAR_GET(cass_fps_limit);
  if (fps <= 0) {
    g_pace_anchor = 0;
    return;
  }
  const int32_t clamped = fps < 10 ? 10 : (fps > 1000 ? 1000 : fps);
  const uint64_t want = (1000000u + uint32_t(clamped) / 2) / uint32_t(clamped);
  if (want != g_pace_interval_us) {
    g_pace_interval_us = want;
    g_pace_anchor = 0;
  }

  const uint64_t now = NowUs();
  if (g_pace_anchor == 0) {
    g_pace_anchor = now;
    g_pace_due = now + g_pace_interval_us;
  } else {
    g_pace_due += g_pace_interval_us;
  }
  if (now >= g_pace_due) {
    // Already missed this slot: release now (no added latency) and move the aim
    // point to the next grid line ahead of the clock.
    const uint64_t slots = (now - g_pace_anchor) / g_pace_interval_us + 1;
    g_pace_due = g_pace_anchor + slots * g_pace_interval_us;
    if (g_pace_due <= now) {
      g_pace_due = now + g_pace_interval_us;
    }
    return;
  }
  const uint64_t target = g_pace_due;
  const int32_t spin_cvar = REXCVAR_GET(cass_fps_limit_spin_us);
  const uint64_t spin_us = spin_cvar <= 0 ? 0 : uint64_t(spin_cvar > 20000 ? 20000 : spin_cvar);

  // Sleep to spin_us short of the slot, then spin. A bare timer lands ~0.5-2 ms
  // late, enough to cross a refresh and bring back the jitter this removes.
  uint64_t remaining = target - now;
  while (remaining > spin_us) {
    const uint64_t wait_us = remaining - spin_us;
    LARGE_INTEGER rel;
    rel.QuadPart = -static_cast<LONGLONG>(wait_us * 10);  // 100 ns units, relative
    if (!g_pace_timer || !SetWaitableTimer(g_pace_timer, &rel, 0, nullptr, nullptr, FALSE)) {
      break;  // no timer: the spin below is correct but hot
    }
    WaitForSingleObject(g_pace_timer, 50);
    const uint64_t t = NowUs();
    if (t >= target) {
      remaining = 0;
      break;
    }
    remaining = target - t;
  }
  while (NowUs() < target) {
    YieldProcessor();
  }
}

// bool D3D12Presenter::RefreshGuestOutputImpl(
//     uint32_t mailbox_index, uint32_t frontbuffer_width, uint32_t frontbuffer_height,
//     std::function<bool(GuestOutputRefreshContext&)> refresher,   <- BY VALUE
//     bool& is_8bpc_out_ref)
// MSVC x64 ABI: a non-trivial by-value argument is passed as the address of a
// caller-made temporary, and the CALLEE destroys it. So the hook takes it as a
// pointer; whatever object is handed to the next function in the chain is that
// function's to destroy, and an object we keep for ourselves we destroy here.
using Refresher = std::function<bool(rex::ui::Presenter::GuestOutputRefreshContext&)>;
using RefreshFn = bool (*)(void* self, uint32_t mailbox_index, uint32_t frontbuffer_w,
                           uint32_t frontbuffer_h, Refresher* refresher, bool* is_8bpc_out);
RefreshFn g_refresh_orig = nullptr;

// The context the D3D12 presenter hands its refresher is always the D3D12 subclass.
void* GuestOutputResource(rex::ui::Presenter::GuestOutputRefreshContext& c) {
  return static_cast<rex::ui::d3d12::D3D12Presenter::D3D12GuestOutputRefreshContext&>(c)
      .resource_uav_capable();
}

bool HookRefresh(void* self, uint32_t mailbox_index, uint32_t frontbuffer_w,
                 uint32_t frontbuffer_h, Refresher* refresher, bool* is_8bpc_out) {
  bool result;
  if (const auto observer = g_observer.load(std::memory_order_acquire)) {
    // The wrapper is handed on as the next callee's by-value argument, so it is
    // built in raw storage and destroyed by that callee, not by this frame. The
    // capture (pointer + fn pointer + two ints) fits std::function's inline
    // buffer: no allocation.
    alignas(Refresher) unsigned char storage[sizeof(Refresher)];
    Refresher* wrapped = ::new (static_cast<void*>(storage))
        Refresher([refresher, observer, frontbuffer_w,
                   frontbuffer_h](rex::ui::Presenter::GuestOutputRefreshContext& c) {
          const bool ok = (*refresher)(c);
          if (ok) {
            observer(GuestOutputResource(c), frontbuffer_w, frontbuffer_h);
          }
          return ok;
        });
    result = g_refresh_orig(self, mailbox_index, frontbuffer_w, frontbuffer_h, wrapped,
                            is_8bpc_out);
    // Our own by-value parameter was not passed on, so it is ours to destroy.
    std::destroy_at(refresher);
  } else {
    // Passed straight through: the next callee owns (and destroys) it.
    result = g_refresh_orig(self, mailbox_index, frontbuffer_w, frontbuffer_h, refresher,
                            is_8bpc_out);
  }
  if (result) {
    // Only a true return means new guest pixels exist to show.
    g_guest_seq.fetch_add(1, std::memory_order_release);
    PaceToGrid();
    rex::glue::FpsOverlayNote(1, g_refresh_iv.Tick());
  }
  return result;
}

bool WriteSlot(void** slot, void* value) {
  DWORD old = 0;
  if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
    return false;
  }
  *slot = value;
  DWORD tmp = 0;
  VirtualProtect(slot, sizeof(void*), old, &tmp);
  FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
  return true;
}

bool InModule(const void* p, HMODULE module) {
  HMODULE owner = nullptr;
  return p && module &&
         GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(p), &owner) &&
         owner == module;
}

// Point the vtable slot holding `target` (the pristine rexruntime export) at
// `replacement`, storing what the slot held in *orig_out. If the export is not
// in the table because another hook in this executable (cass_fsr.cpp hooks
// RefreshGuestOutputImpl the same way) already took the slot, fall back to the
// slot index verified for SDK 923c1a5 and chain onto that hook, so the install
// order of the cass modules does not decide which one works.
bool HookVtable(void** vtable, void* target, void* replacement, size_t known_slot,
                HMODULE runtime, const char* what, void** orig_out) {
  MEMORY_BASIC_INFORMATION mbi{};
  if (!VirtualQuery(vtable, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) {
    REXLOG_WARN("cass pacing: {} vtable {} unreadable", what, static_cast<void*>(vtable));
    return false;
  }
  const uintptr_t end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
  size_t index = kVtableScanSlots;
  for (size_t i = 0; i < kVtableScanSlots; ++i) {
    if (reinterpret_cast<uintptr_t>(vtable + i + 1) > end) {
      break;
    }
    if (vtable[i] == replacement) {
      return true;  // already ours
    }
    if (vtable[i] == target) {
      index = i;
      break;
    }
  }
  bool chained = false;
  if (index == kVtableScanSlots) {
    void* const cur = reinterpret_cast<uintptr_t>(vtable + known_slot + 1) <= end
                          ? vtable[known_slot]
                          : nullptr;
    if (!cur || InModule(cur, runtime) || !InModule(cur, GetModuleHandleW(nullptr))) {
      REXLOG_WARN("cass pacing: {} not found in D3D12Presenter vtable[0..{}]", what,
                  kVtableScanSlots);
      return false;
    }
    index = known_slot;
    chained = true;
  }
  // Publish the call-through target before the slot can route a call to us.
  *orig_out = vtable[index];
  if (!WriteSlot(&vtable[index], replacement)) {
    REXLOG_WARN("cass pacing: {} slot[{}] unprotect failed (gle={})", what, index,
                GetLastError());
    return false;
  }
  REXLOG_INFO("cass pacing: hooked {} (vtable slot {}{})", what, index,
              chained ? ", chained onto an earlier in-process hook" : "");
  return true;
}

}  // namespace

namespace rex::glue {

void SetGuestOutputObserver(GuestOutputObserver observer) {
  g_observer.store(observer, std::memory_order_release);
}

void InstallPacing() {
  static bool installed = false;
  if (installed) {
    return;
  }
  installed = true;

  InstallSystemTuning();
  InstallFpsOverlay();

  if (!QueryPerformanceFrequency(&g_qpc_freq) || g_qpc_freq.QuadPart <= 0) {
    g_qpc_freq.QuadPart = 0;
    REXLOG_WARN("cass pacing: no performance counter; pacing disabled");
    return;
  }
  g_pace_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                        TIMER_ALL_ACCESS);
  const bool high_res = g_pace_timer != nullptr;
  if (!g_pace_timer) {
    g_pace_timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
  }

  HMODULE mod = GetModuleHandleA("rexruntime.dll");
  if (!mod) {
    REXLOG_WARN("cass pacing: rexruntime.dll not loaded; presenter hooks not installed");
    return;
  }
  auto* vtable =
      reinterpret_cast<void**>(GetProcAddress(mod, "??_7D3D12Presenter@d3d12@ui@rex@@6B@"));
  auto* present = reinterpret_cast<void*>(
      GetProcAddress(mod, "?PaintAndPresentImpl@D3D12Presenter@d3d12@ui@rex@@MEAA?"
                          "AW4PaintResult@Presenter@34@_N@Z"));
  auto* refresh = reinterpret_cast<void*>(
      GetProcAddress(mod, "?RefreshGuestOutputImpl@D3D12Presenter@d3d12@ui@rex@@MEAA_"
                          "NIIIV?$function@$$A6A_NAEAVGuestOutputRefreshContext@"
                          "Presenter@ui@rex@@@Z@std@@AEA_N@Z"));
  if (!vtable || !present || !refresh) {
    REXLOG_WARN("cass pacing: missing rexruntime exports (vtable={} present={} refresh={})",
                static_cast<void*>(vtable), present, refresh);
    return;
  }
  // Refresh first: present dedup is only safe when fresh frames are counted.
  // Slot indices are the ones verified for the stock rexruntime.dll (923c1a5);
  // they are only used to chain onto an earlier in-process hook of the slot.
  // The call-through pointers are written by HookVtable before the slot is.
  if (!HookVtable(vtable, refresh, reinterpret_cast<void*>(&HookRefresh), 5, mod,
                  "RefreshGuestOutputImpl", reinterpret_cast<void**>(&g_refresh_orig))) {
    return;
  }
  const bool present_ok =
      HookVtable(vtable, present, reinterpret_cast<void*>(&HookPresent), 6, mod,
                 "PaintAndPresentImpl", reinterpret_cast<void**>(&g_present_orig));
  REXLOG_INFO("cass pacing: installed (present hook {}, high-res timer {})",
              present_ok ? "ok" : "MISSING", high_res ? "yes" : "no (fallback timer)");
}

}  // namespace rex::glue
