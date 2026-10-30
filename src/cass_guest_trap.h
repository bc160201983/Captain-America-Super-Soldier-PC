// Captain America: Super Soldier - ReXGlue port
//
// Host side of tools/patch_guest_traps.py: what recompiled guest code does when it reaches an
// instruction ReXGlue's codegen has no builder for.
//
// The codegen emits such an instruction as REX_UNIMPLEMENTED(addr, "op"), which cass_pch.h
// defines as a C++ throw nobody catches: std::terminate -> abort -> 0xc0000409 (fast fail 7)
// with no line in the log. The patcher rewrites every one of them into a call to:
//
//   CassGuestTrap           "trap" (tw 31,r0,r0, an unconditional debug break: five behind the
//                           game's debug-breakpoint checks, one in its DebugBreak()/assert stub
//                           sub_822D3F20). Logged as a WARN once per site (address + guest lr,
//                           so each asserting caller of the stub is named), then execution
//                           continues at the next instruction, as on a retail console with no
//                           debugger attached and as Xenia does.
//   CassGuestUnimplemented  any other opcode: logs CRITICAL "[FATAL] unimplemented PPC
//                           instruction <op> at <addr>", flushes the log and aborts through
//                           REX_FATAL, so the crash is never silent.
//
// The patcher declares these with the same signatures at the top of each generated file it
// changes. It fails the build if the two extern "C" lines below are not, verbatim, its
// DECL_LINES (extern "C" names link even with a mismatched signature).

#pragma once

#include <cstdint>

extern "C" void CassGuestTrap(uint32_t guest_addr, uint32_t guest_lr);
extern "C" [[noreturn]] void CassGuestUnimplemented(uint32_t guest_addr, const char* opcode);
