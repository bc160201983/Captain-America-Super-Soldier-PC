// Adapted from santiquiroz/gears-of-war-2-pc-port tools/patch_switch_traps.py (BSD-3-Clause, Copyright (c) 2026 Santiago Quiroz Upegui and the gow2-recomp contributors).
// Captain America: Super Soldier - ReXGlue port
//
// Runtime side of tools/patch_switch_traps.py.
//
// ReXGlue turns every guest jump table (mtctr rN; bctr) into a C++ `switch` on the index register
// and closes it with `default: __builtin_trap(); // Switch case out of range`. That is a ud2: if
// an index codegen did not list ever arrives, the process dies with 0xC000001D and nothing in the
// log. The patcher replaces each of those defaults with:
//
//     default:
//         // cass_switch_trap begin: bctr 0x<site> in <func> ...
//         CassSwitchTrapHit(0x<site>u, "<func>", <index>, ctx.ctr.u32);       // WARN, once per target
//         switch (ctx.ctr.u32 & ~3u) {                                          // what bctr really does
//         case 0x<label>u: goto loc_<label>;                                    // every label of <func>
//         ...
//         default: break;
//         }
//         CASS_SWITCH_TRAP_TAIL(0x<site>u, "<func>", <index>);                  // function entry or FATAL
//         return;
//         // cass_switch_trap end
//
// so the guest's real branch target (CTR) decides what happens, exactly as on the console:
//   * CTR is a label codegen emitted in the same function -> goto it (the case codegen missed);
//   * CTR is a registered function entry -> tail call through it, the way codegen emits every
//     other bctr (`REX_CALL_INDIRECT_FUNC(ctx.ctr.u32); return;`);
//   * anything else -> CassSwitchTrapResolve logs a CRITICAL line with the site, the function,
//     the index and CTR, flushes the log and aborts through REX_FATAL. Never a silent ud2.
//
// The generated files include this header (the patcher inserts `#include "cass_switch_trap.h"`
// after their own #include), so the declarations below are the single source of truth for both
// sides. CASS_SWITCH_TRAP_TAIL is only expanded inside generated code: it uses `ctx`, `base` and the
// REX_* dispatch macros from cass_pch.h.

#pragma once

#include <cstdint>

#include <rex/ppc/func.h>

// Both hooks run only on the default path, which every correctly bounded table never takes: marked
// cold so the compiler keeps the default block out of the hot switch's layout.
#if defined(__clang__) || defined(__GNUC__)
#define CASS_SWITCH_TRAP_COLD __attribute__((cold, noinline))
#else
#define CASS_SWITCH_TRAP_COLD
#endif

// Logs (WARN, once per site/target pair, then flushes) that a jump table at guest address `site`
// in `func` was entered with an index codegen did not list, and the CTR the guest is jumping to.
// Thread-safe; a repeat of the same pair on the same thread costs no lock.
extern "C" CASS_SWITCH_TRAP_COLD void CassSwitchTrapHit(uint32_t site, const char* func,
                                                        uint32_t index, uint32_t ctr);

// Returns the host function for `target` when it is a registered guest function entry: `table_func`
// (the per-module dispatch table entry, already looked up by the caller) or the FunctionDispatcher's
// map. Otherwise logs CRITICAL with every detail, flushes and aborts (REX_FATAL); it never returns
// null.
extern "C" CASS_SWITCH_TRAP_COLD PPCFunc* CassSwitchTrapResolve(PPCContext& ctx, uint32_t site,
                                                                const char* func, uint32_t index,
                                                                uint32_t target,
                                                                PPCFunc* table_func);

// bctr ignores the low two bits of CTR. The dispatch-table lookup mirrors REX_CALL_INDIRECT_FUNC
// (cass_pch.h) so a target is called through the same entry other indirect branches use.
#define CASS_SWITCH_TRAP_TAIL(site, func, index)                                                  \
  do {                                                                                            \
    const uint32_t cass_st_target_ = ctx.ctr.u32 & ~3u;                                           \
    PPCFunc* cass_st_func_ = nullptr;                                                             \
    if ((uint32_t)(cass_st_target_ - REX_CODE_BASE) < REX_CODE_SIZE + REX_THUNK_RESERVE_SIZE) {   \
      cass_st_func_ = REX_LOOKUP_FUNC(base, cass_st_target_);                                     \
    }                                                                                             \
    cass_st_func_ =                                                                               \
        CassSwitchTrapResolve(ctx, (site), (func), (index), cass_st_target_, cass_st_func_);      \
    ctx.last_indirect_target = cass_st_target_;                                                   \
    cass_st_func_(ctx, base);                                                                     \
  } while (0)
