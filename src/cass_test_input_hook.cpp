// Adapted from santiquiroz/gears-of-war-2-pc-port android/gamelibs/gow2_android_glue.cpp (BSD-3-Clause, Copyright (c) 2026 Santiago Quiroz Upegui and the gow2-recomp contributors).
//
// cass - the guest's XamInputGetState, wrapped so the scripted test input (cass_test_input.h) is
// merged into what it returns. With the test input off this is a plain forward to the runtime.
//
// How it binds: the recompiled code declares __imp__XamInputGetState (generated/default/
// cass_funcs.h) and normally gets rexruntime.dll's export of that name from the import library.
// Defining the symbol here, in cass.exe, binds every reference to this definition instead (the
// linker only pulls an import-library member for a symbol that is still undefined):
//   - the only call, in sub_8249F0E8 (the game's XInputGetState(user, state) helper, flags = 1),
//     whose only caller is the per-user pad poll sub_822B3B58 (it treats r3 == 0 as "connected");
//   - the function-table entry for the import thunk 0x8286626C (cass_init.cpp, cass_register.cpp).
// Neither the code nor the data of the game take 0x8286626C's or sub_8249F0E8's address, so no
// indirect call goes around this. The runtime's implementation is found by name (GetProcAddress)
// at the first call and always runs first, unchanged; the SDK's focus rule (input ignored while
// the window is unfocused) is applied inside it, and the scripted state is applied afterwards.
// Checked in a full relink of cass.exe: its import table no longer lists __imp__XamInputGetState.
//
// gow2 does the same on Android (libgow2.so defines the export); its Windows build instead
// rewrites rexruntime.dll's export thunk, which needs a jmp thunk the stock DLL does not promise.

#include "cass_test_input.h"

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <cstdint>
#include <iterator>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

namespace {

constexpr const char* kExport = "__imp__XamInputGetState";
constexpr uint32_t kErrorDeviceNotConnected = 0x48F;

PPCFunc* AsPPCFunc(FARPROC fn) { return reinterpret_cast<PPCFunc*>(reinterpret_cast<void*>(fn)); }

PPCFunc* FindRuntimeExport() {
  for (const wchar_t* name : {L"rexruntime.dll", L"rexruntimerd.dll", L"rexruntimed.dll"}) {
    if (HMODULE mod = GetModuleHandleW(name)) {
      if (FARPROC fn = GetProcAddress(mod, kExport)) return AsPPCFunc(fn);
    }
  }
  // A runtime DLL under another name: any loaded module but cass.exe that exports it.
  HMODULE self = GetModuleHandleW(nullptr);
  HMODULE mods[1024];
  DWORD needed = 0;
  if (K32EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) {
    const DWORD count = std::min<DWORD>(needed / sizeof(HMODULE), DWORD(std::size(mods)));
    for (DWORD i = 0; i < count; ++i) {
      if (mods[i] == self) continue;
      if (FARPROC fn = GetProcAddress(mods[i], kExport)) return AsPPCFunc(fn);
    }
  }
  return nullptr;
}

PPCFunc* RuntimeXamInputGetState() {
  static PPCFunc* const original = [] {
    PPCFunc* fn = FindRuntimeExport();
    if (!fn) {
      REXLOG_ERROR("cass test input: the runtime's {} export was not found; every pad reads as "
                   "disconnected",
                   kExport);
    }
    return fn;
  }();
  return original;
}

}  // namespace

// XamInputGetState(user_index r3, flags r4, XINPUT_STATE* r5); r3 holds the result afterwards.
extern "C" REX_FUNC(__imp__XamInputGetState) {
  const uint32_t user_index = ctx.r3.u32;
  const uint32_t state_addr = ctx.r5.u32;
  if (PPCFunc* original = RuntimeXamInputGetState()) {
    original(ctx, base);
  } else {
    ctx.r3.u64 = kErrorDeviceNotConnected;
  }
  if (!rex::glue::TestInputActive()) return;
  uint32_t result = ctx.r3.u32;
  rex::glue::ApplyTestInput(base, user_index, state_addr, &result);
  ctx.r3.u64 = result;
}
