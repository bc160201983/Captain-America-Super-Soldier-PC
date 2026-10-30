// Adapted from gowj (Gears of War: Judgment ReXGlue port) src/gowj_system_tuning.cpp (BSD-3-Clause, Copyright (c) 2026 the contributors to this project).
//
// cass - process-level tuning.
//
// Opts the process out of Windows power throttling (EcoQoS) and timer-resolution
// throttling. On laptops, handhelds and hybrid-core CPUs Windows can otherwise park
// the game's threads on efficiency cores and coarsen its timers, which the frame
// limiter's waitable timer depends on.

#include "cass_system_tuning.h"

#include <windows.h>

#include <rex/logging.h>

namespace rex::glue {

void InstallSystemTuning() {
  static bool done = false;
  if (done) {
    return;
  }
  done = true;
  // PROCESS_POWER_THROTTLING_STATE, with the constants spelled out for older SDK headers:
  // Version 1, control EXECUTION_SPEED (0x1) | IGNORE_TIMER_RESOLUTION (0x4), state 0 = off.
  struct State {
    ULONG Version;
    ULONG ControlMask;
    ULONG StateMask;
  } st{1, 0x1 | 0x4, 0};
  const BOOL ok =
      SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &st, sizeof(st));
  REXLOG_INFO("cass: power throttling opt-out {}", ok ? "applied" : "not supported here");
}

}  // namespace rex::glue
