// Adapted from santiquiroz/gears-of-war-2-pc-port tools/patch_switch_traps.py (BSD-3-Clause, Copyright (c) 2026 Santiago Quiroz Upegui and the gow2-recomp contributors).
// Captain America: Super Soldier - ReXGlue port
//
// Host side of tools/patch_switch_traps.py: what a generated jump table does when the guest index
// is not in codegen's case list. See cass_switch_trap.h for the generated shape and the rules.
//
// Both functions are cold: the default of a generated switch is unreachable for every jump table
// whose bounds check codegen read correctly, so reaching here at all is worth a log line.

#include "cass_switch_trap.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_set>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>

namespace {

// Distinct (site, target) pairs already logged, per kind. Capped so a corrupted CTR fed through a
// hot jump table cannot flood the log; the cap never suppresses the fatal line.
constexpr size_t kMaxLogged = 256;

struct LoggedPairs {
  std::mutex mutex;
  std::unordered_set<uint64_t> seen;
  bool cap_reported = false;
};

LoggedPairs g_hits;   // CassSwitchTrapHit: default entered
LoggedPairs g_calls;  // CassSwitchTrapResolve: CTR followed into a function entry

enum class Verdict { kSkip, kLog, kCapReached };

uint64_t Key(uint32_t site, uint32_t target) { return (uint64_t(site) << 32) | target; }

// Decides under the lock; the caller logs after it is released, so no log call (or flush) ever runs
// while a guest thread holds this mutex.
Verdict FirstTime(LoggedPairs& pairs, uint64_t key) {
  std::lock_guard<std::mutex> lock(pairs.mutex);
  if (pairs.seen.count(key)) return Verdict::kSkip;
  if (pairs.seen.size() >= kMaxLogged) {
    if (pairs.cap_reported) return Verdict::kSkip;
    pairs.cap_reported = true;
    return Verdict::kCapReached;
  }
  pairs.seen.insert(key);
  return Verdict::kLog;
}

void Flush() {
  if (auto logger = rex::GetLogger()) logger->flush();
}

}  // namespace

extern "C" void CassSwitchTrapHit(uint32_t site, const char* func, uint32_t index, uint32_t ctr) {
  const uint32_t target = ctr & ~3u;
  const uint64_t key = Key(site, target);
  // An under-counted table in a hot loop repeats the same (site, target) pair on the same thread:
  // skip the lock for it. (site << 32 | target) never equals ~0 because target has its low bits
  // clear, so the initial value cannot match a real pair.
  thread_local uint64_t t_last_key = ~uint64_t(0);
  if (key == t_last_key) return;
  t_last_key = key;
  switch (FirstTime(g_hits, key)) {
    case Verdict::kSkip:
      return;
    case Verdict::kCapReached:
      REXLOG_WARN(
          "switch trap: {} distinct jump-table misses logged; further new ones are not logged (a "
          "fatal one still is)",
          kMaxLogged);
      Flush();
      return;
    case Verdict::kLog:
      break;
  }
  const char* name = func ? func : "?";
  REXLOG_WARN(
      "switch trap: jump table at 0x{:08X} in {} got index {} (0x{:X}), which codegen did not list; "
      "guest CTR=0x{:08X}. A label of {} is jumped to, a registered function entry is tail-called, "
      "anything else is fatal.",
      site, name, index, index, ctr, name);
  Flush();
}

extern "C" PPCFunc* CassSwitchTrapResolve(PPCContext& ctx, uint32_t site, const char* func,
                                          uint32_t index, uint32_t target, PPCFunc* table_func) {
  const char* name = func ? func : "?";
  PPCFunc* fn = table_func;
  const char* how = "dispatch table";
  if (!fn) {
    // Same lookup ResolveIndirectFunction does, minus its fallback to the invalid-function stub:
    // FunctionDispatcher::GetFunction returns null for an address nothing registered.
    if (rex::Runtime* rt = rex::Runtime::instance()) {
      if (rex::runtime::FunctionDispatcher* fd = rt->function_dispatcher()) {
        fn = fd->GetFunction(target);
        how = "function dispatcher";
      }
    }
  }
  if (fn) {
    switch (FirstTime(g_calls, Key(site, target))) {
      case Verdict::kSkip:
        break;
      case Verdict::kCapReached:
        REXLOG_WARN(
            "switch trap: {} distinct jump-table tail calls logged; further new ones are not logged",
            kMaxLogged);
        Flush();
        break;
      case Verdict::kLog:
        REXLOG_WARN(
            "switch trap: jump table at 0x{:08X} in {} (index {}): CTR=0x{:08X} is a registered "
            "function entry ({}), tail-calling it as bctr would",
            site, name, index, target, how);
        Flush();
        break;
    }
    return fn;
  }
  REX_FATAL(
      "switch trap: jump table at 0x{:08X} in {} got index {} (0x{:X}), which codegen did not "
      "list, and the guest branch target CTR=0x{:08X} is neither a label codegen emitted in {} nor "
      "a registered function entry (LR=0x{:08X}, r1=0x{:08X}, r3=0x{:08X}). Check the jump table "
      "bounds at 0x{:08X} in out/image/image.bin, add a function or jump-table hint, re-run codegen.",
      site, name, index, index, target, name, static_cast<uint32_t>(ctx.lr), ctx.r1.u32,
      ctx.r3.u32, site);
}
