// Captain America: Super Soldier - ReXGlue port
//
// Guest debug breaks and unimplemented PPC instructions (see cass_guest_trap.h and
// tools/patch_guest_traps.py).
//
// The game has six unconditional "trap" instructions (tw 31,r0,r0 = 0x7FE00008):
//   0x822CFCC4, 0x822D0034  behind sub_822CC7E0 (debug-breakpoint registry at 0x82906558)
//   0x822D06C0, 0x822D1180  behind sub_822CC720 (same registry)
//   0x822D1368              behind sub_822CC7E0
//   0x822D3F20              sub_822D3F20 = "trap; blr", the game's DebugBreak()/assert stub. It
//                           is live: 56 "bl" and 2 tail-call ("b") sites call it when an
//                           assertion fails (e.g. a null callback, an uninitialised renderer
//                           flag), so its trap address alone does not say which assertion fired.
// The registry is empty in the retail game, so the first five should not fire; if any trap
// fires, the console (no debugger attached) and Xenia both log it and carry on, and so do we.
//
// A "site" is (trap address, guest lr). At the five guarded traps lr is the return address of
// the registry-check bl just before them (constant per site); at the DebugBreak stub it is the
// return address of the asserting caller's bl (for the two tail calls, of the caller's caller),
// so every assertion is logged on its own the first time it fires.

#include "cass_guest_trap.h"

#include <rex/logging.h>

#include <cstdio>
#include <mutex>
#include <unordered_map>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {

// Both leaked on purpose: guest threads can still hit a trap while the process runs its static
// destructors at exit.
std::mutex& TrapMutex() {
  static auto* mutex = new std::mutex();
  return *mutex;
}

// Hits per site, keyed (trap address << 32) | guest lr.
std::unordered_map<uint64_t, uint64_t>& TrapHits() {
  static auto* hits = new std::unordered_map<uint64_t, uint64_t>();
  return *hits;
}

void FlushLog() {
  if (auto logger = rex::GetLogger()) logger->flush();
}

}  // namespace

extern "C" void CassGuestTrap(uint32_t guest_addr, uint32_t guest_lr) {
  const uint64_t key = (uint64_t{guest_addr} << 32) | guest_lr;
  // The return address of the last bl, i.e. the asserting call for the DebugBreak stub.
  const uint32_t last_bl = guest_lr >= 4 ? guest_lr - 4 : 0;
  std::lock_guard<std::mutex> lock(TrapMutex());
  const uint64_t n = ++TrapHits()[key];
  if (n == 1) {
    REXLOG_WARN(
        "Guest trap (tw 31,r0,r0) at 0x{:08X}, lr 0x{:08X} (last bl at 0x{:08X}): the game hit "
        "a debug break / failed assertion. Ignored as on a console without a debugger (and as "
        "in Xenia); execution continues at the next instruction. Logged once per site "
        "(address + lr).",
        guest_addr, guest_lr, last_bl);
    FlushLog();  // if a crash follows, this line must already be on disk
  } else if (n == 100 || n == 10000 || n == 1000000) {
    REXLOG_WARN("Guest trap at 0x{:08X}, lr 0x{:08X} hit {} times (still ignored)", guest_addr,
                guest_lr, n);
  }
}

extern "C" void CassGuestUnimplemented(uint32_t guest_addr, const char* opcode) {
  if (!opcode) opcode = "?";
  // Also outside the log, in case it is not set up yet (or this is reached at exit).
  char line[160];
  std::snprintf(line, sizeof(line),
                "cass: [FATAL] unimplemented PPC instruction %s at 0x%08X\n", opcode,
                static_cast<unsigned>(guest_addr));
  std::fputs(line, stderr);
  std::fflush(stderr);
#ifdef _WIN32
  OutputDebugStringA(line);
#endif
  REX_FATAL(
      "unimplemented PPC instruction {} at 0x{:08X}: the ReXGlue codegen has no builder for it "
      "(see tools/patch_guest_traps.py); cannot continue",
      opcode, guest_addr);
}
