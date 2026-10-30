// Adapted from santiquiroz/gears-of-war-2-pc-port src/gow2_crash_watch.cpp (BSD-3-Clause, Copyright (c) 2026 Santiago Quiroz Upegui and the gow2-recomp contributors).
//
// cass - crash guard. See cass_crash_guard.h for what is caught and what a crash produces.
//
// Carried over from the Gears 2 crash watch: a minidump beside a plain-Win32 text report with
// fallback dump flag sets (the 4 GB guest reservation makes some sets fail with ERROR_NOACCESS),
// module+offset for addresses outside recompiled code, guest function attribution through the
// codegen's PPCFuncMappings table. Not carried over: the overread guard (Gears-specific).
// New here: the SIGABRT / terminate / invalid-parameter / purecall hooks, the C++ throw recorder,
// an unwinder-based call stack, guest register state, the session-log lines and the dialog.
//
// Rules this file keeps:
//   * Nothing reports at first chance. The only vectored handler (the throw recorder) is appended
//     last, looks at C++ throws only and always continues the search.
//   * One report per process. The first failing thread owns it; a second thread that fails while
//     it runs waits (so it cannot end the process under the dump writer); a fault on the owning
//     thread or the reporter thread during the report falls through to the default fatal path.
//   * The crashing thread does as little as possible: it records what it knows, wakes the reporter
//     thread and waits. The report, the dump and the dialog run on the reporter thread, which has
//     a full stack (a stack-overflowing thread has a few KB) and is not holding whatever lock the
//     crashing thread holds.
//   * The text report is built in static buffers with snprintf and written with CreateFileW, so a
//     wedged logger or allocator cannot prevent it. The logger is driven from a helper thread with
//     a timeout; if it does not answer, the report goes on without it.

#include "cass_crash_guard.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// dbghelp.h needs windows.h first.
#include <dbghelp.h>

#include <atomic>
#include <csignal>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <exception>
#include <filesystem>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <typeinfo>

#include <rex/logging.h>
#include <rex/ppc/func.h>
#include <rex/runtime.h>
#include <rex/system/thread_state.h>
#include <rex/system/xmemory.h>

#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "user32.lib")

// Exported by VCRUNTIME140 (the compiler's pure-call target); only the crash test calls it.
extern "C" int __cdecl _purecall();

namespace {

// ------------------------------------------------------------------------------------------------
// Constants and shared state
// ------------------------------------------------------------------------------------------------

constexpr DWORD kCppExceptionCode = 0xE06D7363;  // MSVC-ABI C++ throw ('msc' | 0xE0000000)
constexpr ULONG_PTR kEhMagic1 = 0x19930520;
constexpr ULONG_PTR kEhMagic2 = 0x19930521;
constexpr ULONG_PTR kEhMagic3 = 0x19930522;
constexpr DWORD kStatusFailFast = 0xC0000409;     // what __fastfail raises
constexpr ULONG_PTR kFastFailInvalidArg = 5;      // FAST_FAIL_INVALID_ARG (_invoke_watson)
constexpr ULONG_PTR kFastFailFatalAppExit = 7;    // FAST_FAIL_FATAL_APP_EXIT (abort)
constexpr ULONGLONG kOwnerWaitCapMs = 90000;      // longest wait for a reporter stuck before the dialog
constexpr DWORD kLoggerWaitMs = 3000;             // longest wait for the logger helper per batch
constexpr DWORD kRearmPeriodMs = 2000;
constexpr int kKeepDumps = 10;
constexpr int kMaxFrames = 48;

enum class Kind { kException, kTerminate, kAbort, kInvalidParameter, kPureCall };
enum Stage : int { kStageIdle, kStageText, kStageLog, kStageDump, kStageDialog, kStageDone };

// Type and what() of the last C++ exception thrown on a thread, copied at throw time.
struct ThrowRecord {
  ULONGLONG tick;
  unsigned count;
  char type[128];
  char what[384];
};

// Everything the crashing thread hands to the reporter. Written only by the thread that owns the
// report (after it won g_owner), read by the reporter after g_request is signalled.
struct Request {
  Kind kind;
  DWORD tid;
  EXCEPTION_POINTERS* ep;
  ULONG_PTR stack_low, stack_high;
  ULONGLONG tick;
  bool has_guest;
  uint32_t guest_tid, r1, r3, r4, r5, ctr, last_indirect;
  uint64_t lr;
  ThrowRecord last_throw;
  char summary[384];  // one line for the dialog (kinds other than kException)
  char detail[2048];  // extra report lines (kinds other than kException)
};

thread_local ThrowRecord t_last_throw;
thread_local bool t_terminate_hooked;
thread_local bool t_in_filter;

// Filled once by InstallCrashGuard, read-only afterwards.
wchar_t g_logs_dir[MAX_PATH];     // <exe dir>\logs
wchar_t g_session_log[MAX_PATH];  // rex::LoggingConfig().log_file, when it names a file
wchar_t g_log_dir_cfg[MAX_PATH];  // rex::LoggingConfig().log_dir
uintptr_t g_exe_base = 0, g_exe_end = 0;
const RUNTIME_FUNCTION* g_pdata = nullptr;  // the exe's .pdata, sorted by BeginAddress
size_t g_pdata_count = 0;
bool g_dialog_enabled = true;
DWORD g_install_tid = 0;

std::atomic<DWORD> g_owner{0};  // thread id of the thread that owns the report
std::atomic<int> g_stage{kStageIdle};
std::atomic<DWORD> g_reporter_tid{0};
std::atomic<DWORD> g_logger_tid{0};
// The helper threads' handles stay open for the life of the process: ExitProcess ends every other
// thread before it runs DLL_PROCESS_DETACH, so an abort() or fault in a DLL's static destructor
// finds them gone, and waiting on them would only hang the exit (Finish checks before it waits).
HANDLE g_reporter_thread = nullptr;
HANDLE g_logger_thread = nullptr;
using ShutdownInProgressFn = BOOLEAN(NTAPI*)();
ShutdownInProgressFn g_shutdown_in_progress = nullptr;  // ntdll!RtlDllShutdownInProgress
bool g_exiting = false;  // set by the owner: the report runs during process exit (no logger/dialog)
std::atomic<LPTOP_LEVEL_EXCEPTION_FILTER> g_prev_filter{nullptr};
std::atomic<_crt_signal_t> g_prev_sigabrt{nullptr};
HANDLE g_request = nullptr;      // auto-reset: owner -> reporter
HANDLE g_done = nullptr;         // manual-reset: reporter -> every waiting thread
HANDLE g_log_request = nullptr;  // auto-reset: reporter -> logger helper
HANDLE g_log_done = nullptr;     // auto-reset: logger helper -> reporter
bool g_logger_stuck = false;     // reporter only

Request g_req;
EXCEPTION_RECORD g_synth_record;
CONTEXT g_synth_context;  // DECLSPEC_ALIGN(16) by declaration, as RtlCaptureContext needs
EXCEPTION_POINTERS g_synth_pointers;
CONTEXT g_walk_context;  // reporter only

// Reporter-only buffers.
char g_text[64 * 1024];
size_t g_text_len = 0;
char g_summary[512];
wchar_t g_txt_path[MAX_PATH];
wchar_t g_dmp_path[MAX_PATH];
wchar_t g_log_path[MAX_PATH];
wchar_t g_message[4096];
const char* g_log_src = nullptr;
size_t g_log_len = 0;

LONG WINAPI OnUnhandledException(EXCEPTION_POINTERS* ep);
void __cdecl OnSigAbrt(int);
[[noreturn]] void __cdecl OnTerminate();

// ------------------------------------------------------------------------------------------------
// Small helpers (no heap)
// ------------------------------------------------------------------------------------------------

// Bounded copy that turns control characters into spaces (exception texts go into one-line logs).
void CopyLine(char* out, size_t cap, const char* in, size_t max_in) {
  if (!cap) return;
  size_t i = 0;
  for (; in && i < max_in && in[i] && i + 1 < cap; ++i) {
    const unsigned char c = static_cast<unsigned char>(in[i]);
    out[i] = c < 0x20 ? ' ' : static_cast<char>(c);
  }
  out[i] = 0;
}

void ToUtf8(const wchar_t* in, char* out, int cap) {
  out[0] = 0;
  if (!in || !in[0]) return;
  const int n = WideCharToMultiByte(CP_UTF8, 0, in, -1, out, cap, nullptr, nullptr);
  if (n <= 0) out[0] = 0;
  out[cap - 1] = 0;
}

void ToWide(const char* in, wchar_t* out, int cap) {
  out[0] = 0;
  if (!in || !in[0]) return;
  const int n = MultiByteToWideChar(CP_UTF8, 0, in, -1, out, cap);
  if (n <= 0) out[0] = 0;
  out[cap - 1] = 0;
}

bool ThreadAlive(HANDLE h) { return h && WaitForSingleObject(h, 0) == WAIT_TIMEOUT; }

// ExitProcess has begun: the other threads are gone and DLLs are being detached.
bool ProcessExiting() {
  if (g_shutdown_in_progress && g_shutdown_in_progress()) return true;
  return g_reporter_thread && !ThreadAlive(g_reporter_thread);
}

bool Readable(const void* p, size_t len) {
  MEMORY_BASIC_INFORMATION mbi{};
  if (!p || !VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
  if ((mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) return false;
  return reinterpret_cast<uintptr_t>(p) + len <=
         reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
}

struct Text {
  char* buf;
  size_t cap;
  size_t len;
  void Add(const char* fmt, ...) {
    if (len + 1 >= cap) return;
    va_list ap;
    va_start(ap, fmt);
    const int n = _vsnprintf_s(buf + len, cap - len, _TRUNCATE, fmt, ap);
    va_end(ap);
    len += n >= 0 ? static_cast<size_t>(n) : strnlen(buf + len, cap - len);
  }
};

bool WriteFileAll(const wchar_t* path, const char* data, size_t len, bool append) {
  HANDLE h = CreateFileW(path, append ? FILE_APPEND_DATA : GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                         append ? OPEN_ALWAYS : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  const BOOL ok = WriteFile(h, data, static_cast<DWORD>(len), &written, nullptr);
  CloseHandle(h);
  return ok && written == len;
}

bool IsFile(const wchar_t* path) {
  if (!path[0]) return false;
  const DWORD a = GetFileAttributesW(path);
  return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// Newest file matching dir\pattern; false when there is none.
bool NewestFile(const wchar_t* dir, const wchar_t* pattern, wchar_t* out, size_t cap) {
  wchar_t query[MAX_PATH];
  if (_snwprintf_s(query, _TRUNCATE, L"%ls\\%ls", dir, pattern) < 0) return false;
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW(query, &fd);
  if (h == INVALID_HANDLE_VALUE) return false;
  FILETIME best{};
  bool found = false;
  do {
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
    if (!found || CompareFileTime(&fd.ftLastWriteTime, &best) > 0) {
      best = fd.ftLastWriteTime;
      found = _snwprintf_s(out, cap, _TRUNCATE, L"%ls\\%ls", dir, fd.cFileName) >= 0;
    }
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  return found;
}

const char* ExceptionName(DWORD code) {
  switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "access violation";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
    case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
    case EXCEPTION_IN_PAGE_ERROR: return "in-page error";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer divide by zero";
    case EXCEPTION_INT_OVERFLOW: return "integer overflow";
    case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
    case EXCEPTION_BREAKPOINT: return "breakpoint";
    case EXCEPTION_SINGLE_STEP: return "single step";
    case EXCEPTION_DATATYPE_MISALIGNMENT: return "datatype misalignment";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "array bounds exceeded";
    case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "noncontinuable exception";
    case EXCEPTION_INVALID_DISPOSITION: return "invalid disposition";
    case EXCEPTION_FLT_DENORMAL_OPERAND:
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:
    case EXCEPTION_FLT_INEXACT_RESULT:
    case EXCEPTION_FLT_INVALID_OPERATION:
    case EXCEPTION_FLT_OVERFLOW:
    case EXCEPTION_FLT_STACK_CHECK:
    case EXCEPTION_FLT_UNDERFLOW: return "floating point exception";
    case 0xC0000374: return "heap corruption";
    case kStatusFailFast: return "fail fast";
    case 0xC0000417: return "invalid C runtime parameter";
    case kCppExceptionCode: return "C++ exception";
    default: return "exception";
  }
}

// ------------------------------------------------------------------------------------------------
// C++ exception decoding (MSVC x64 ABI: ThrowInfo and friends hold image-relative offsets)
// ------------------------------------------------------------------------------------------------

struct EhPmd {
  int mdisp, pdisp, vdisp;
};
struct EhCatchableType {
  unsigned properties;
  int type_rva;
  EhPmd this_displacement;
  int size_or_offset;
  int copy_function_rva;
};
struct EhCatchableTypeArray {
  int count;
  int type_rvas[1];
};
struct EhThrowInfo {
  unsigned attributes;
  int unwind_rva;
  int forward_compat_rva;
  int catchable_type_array_rva;
};
struct EhTypeDescriptor {
  const void* vftable;
  void* spare;
  char name[1];
};

// ".?AVruntime_error@std@@" -> "std::runtime_error". Templates, back-references and non-class types
// are left decorated, which is still unambiguous.
void Undecorate(const char* decorated, char* out, size_t cap) {
  const size_t n = strnlen(decorated, 256);
  if (n > 6 && decorated[0] == '.' && decorated[1] == '?' && decorated[2] == 'A' &&
      (decorated[3] == 'V' || decorated[3] == 'U') && !memchr(decorated + 4, '?', n - 4)) {
    const char* part[8];
    size_t part_len[8];
    int parts = 0;
    const char* p = decorated + 4;
    const char* end = decorated + n;
    bool ok = true;
    while (p < end && *p != '@') {
      const char* at = static_cast<const char*>(memchr(p, '@', static_cast<size_t>(end - p)));
      if (!at || parts == 8 || (at - p == 1 && *p >= '0' && *p <= '9')) {
        ok = false;
        break;
      }
      part[parts] = p;
      part_len[parts] = static_cast<size_t>(at - p);
      ++parts;
      p = at + 1;
    }
    if (ok && parts > 0 && p < end && *p == '@') {
      size_t len = 0;
      for (int i = parts - 1; i >= 0; --i) {
        if (i != parts - 1 && len + 2 < cap) {
          out[len++] = ':';
          out[len++] = ':';
        }
        for (size_t k = 0; k < part_len[i] && len + 1 < cap; ++k) out[len++] = part[i][k];
      }
      out[len] = 0;
      return;
    }
  }
  CopyLine(out, cap, decorated, n);
}

const uint8_t* AdjustThis(const uint8_t* object, const EhPmd& pmd) {
  const uint8_t* p = object + pmd.mdisp;
  if (pmd.pdisp >= 0) {
    const uint8_t* vbtable = *reinterpret_cast<const uint8_t* const*>(object + pmd.pdisp);
    p += *reinterpret_cast<const int*>(vbtable + pmd.vdisp) + pmd.pdisp;
  }
  return p;
}

// SEH note for every *Unguarded worker below: without asynchronous EH (the host sources are not
// built with /EHa), clang's __try only covers faults raised inside calls made from its body, not
// loads and stores it inlines there. So each risky read lives in a noinline worker that the
// guarded wrapper calls.
__declspec(noinline) bool DescribeCppExceptionUnguarded(const EXCEPTION_RECORD* rec, char* type,
                                                        size_t type_cap, char* what, size_t what_cap) {
  if (rec->ExceptionCode != kCppExceptionCode || rec->NumberParameters < 4) return false;
  const ULONG_PTR magic = rec->ExceptionInformation[0];
  if (magic != kEhMagic1 && magic != kEhMagic2 && magic != kEhMagic3) return false;
  const auto* object = reinterpret_cast<const uint8_t*>(rec->ExceptionInformation[1]);
  const auto* info = reinterpret_cast<const EhThrowInfo*>(rec->ExceptionInformation[2]);
  const uintptr_t image = rec->ExceptionInformation[3];
  if (!info || !image || !info->catchable_type_array_rva) return false;  // "throw;" has no ThrowInfo
  const auto* types =
      reinterpret_cast<const EhCatchableTypeArray*>(image + info->catchable_type_array_rva);
  const uint8_t* std_exception = nullptr;
  for (int i = 0; i < types->count && i < 64; ++i) {
    const auto* ct = reinterpret_cast<const EhCatchableType*>(image + types->type_rvas[i]);
    const auto* td = reinterpret_cast<const EhTypeDescriptor*>(image + ct->type_rva);
    if (i == 0) Undecorate(td->name, type, type_cap);  // the thrown (most derived) type comes first
    if (!std_exception && object && strcmp(td->name, ".?AVexception@std@@") == 0) {
      std_exception = AdjustThis(object, ct->this_displacement);
    }
  }
  if (std_exception) {
    const char* w = reinterpret_cast<const std::exception*>(std_exception)->what();
    CopyLine(what, what_cap, w, what_cap - 1);
  }
  return type[0] != 0;
}

// The object must still be alive: first chance, or its thread blocked in the unhandled filter.
bool DescribeCppException(const EXCEPTION_RECORD* rec, char* type, size_t type_cap, char* what,
                          size_t what_cap) {
  type[0] = 0;
  what[0] = 0;
  __try {
    return DescribeCppExceptionUnguarded(rec, type, type_cap, what, what_cap);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return type[0] != 0;
  }
}

// ------------------------------------------------------------------------------------------------
// Address attribution: module + offset, and the recompiled guest function
// ------------------------------------------------------------------------------------------------

struct GuestHit {
  uint32_t guest;
  uintptr_t host;
  bool exact;
};

// &sub_X is the function body, except under incremental linking, where it is a "jmp rel32" thunk.
uintptr_t FollowJmpThunk(uintptr_t p) {
  const auto* b = reinterpret_cast<const uint8_t*>(p);
  return b[0] == 0xE9 ? p + 5 + static_cast<intptr_t>(*reinterpret_cast<const int32_t*>(b + 1)) : p;
}

// The host function containing addr starts at its .pdata BeginAddress (followed back through
// chained unwind info); the PPCFuncMappings entry with exactly that host address names the guest
// function, and no entry means host code of cass.exe (no guest name is invented for it). Only a
// function without unwind data - a frameless leaf, which recompiled leaves can be - is attributed
// to the nearest mapping below it (within 64 KB, with no other function in between), marked
// approximate.
__declspec(noinline) bool FindGuestUnguarded(uintptr_t addr, GuestHit* hit) {
  if (addr < g_exe_base || addr >= g_exe_end) return false;
  DWORD64 image_base = 0;
  PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(addr, &image_base, nullptr);
  if (fe && image_base == g_exe_base) {
    for (int i = 0; i < 8; ++i) {
      if (fe->UnwindData & 1) {  // indirect entry
        fe = reinterpret_cast<PRUNTIME_FUNCTION>(image_base + (fe->UnwindData & ~1u));
        continue;
      }
      const auto* ui = reinterpret_cast<const uint8_t*>(image_base + fe->UnwindData);
      if (!((ui[0] >> 3) & UNW_FLAG_CHAININFO)) break;
      const unsigned codes = ui[2];
      fe = reinterpret_cast<PRUNTIME_FUNCTION>(const_cast<uint8_t*>(ui) + 4 + ((codes + 1) & ~1u) * 2);
    }
    const uintptr_t start = static_cast<uintptr_t>(image_base) + fe->BeginAddress;
    for (const PPCFuncMapping* m = PPCFuncMappings; m->guest || m->host; ++m) {
      const auto host = reinterpret_cast<uintptr_t>(m->host);
      if (host && (host == start || FollowJmpThunk(host) == start)) {
        *hit = GuestHit{static_cast<uint32_t>(m->guest), start, true};
        return true;
      }
    }
    return false;
  }
  GuestHit best{0, 0, false};
  for (const PPCFuncMapping* m = PPCFuncMappings; m->guest || m->host; ++m) {
    if (!m->host) continue;
    const uintptr_t host = FollowJmpThunk(reinterpret_cast<uintptr_t>(m->host));
    if (host <= addr && host > best.host) best = GuestHit{static_cast<uint32_t>(m->guest), host, false};
  }
  if (!best.host || addr - best.host > 0x10000) return false;  // too far to be the same leaf
  // Any function with unwind data starting between that mapping and addr (host code linked
  // beside the recompiled code: SDL, spdlog, cass_*.cpp) means addr is not in that guest leaf.
  const DWORD addr_rva = static_cast<DWORD>(addr - g_exe_base);
  size_t lo = 0, hi = g_pdata_count;
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    if (g_pdata[mid].BeginAddress <= addr_rva) lo = mid + 1; else hi = mid;
  }
  if (lo && g_exe_base + g_pdata[lo - 1].BeginAddress > best.host) return false;
  *hit = best;
  return true;
}

bool FindGuest(uintptr_t addr, GuestHit* hit) {
  __try {
    return FindGuestUnguarded(addr, hit);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// "cass.exe+0x123456  guest sub_822CFC80+0x44". `lookup` is the address to attribute (a return
// address minus one, so a call at the very end of a function is not credited to the next one).
void DescribeLocation(uintptr_t addr, uintptr_t lookup, char* out, size_t cap) {
  Text t{out, cap, 0};
  out[0] = 0;
  HMODULE mod = nullptr;
  if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(lookup), &mod) && mod) {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(mod, path, MAX_PATH - 1);
    const wchar_t* base = wcsrchr(path, L'\\');
    char name[MAX_PATH];
    ToUtf8(base ? base + 1 : path, name, sizeof(name));
    t.Add("%s+0x%llX", name[0] ? name : "?",
          static_cast<unsigned long long>(addr - reinterpret_cast<uintptr_t>(mod)));
  } else {
    t.Add("0x%016llX (not in any loaded module)", static_cast<unsigned long long>(addr));
  }
  GuestHit g;
  if (FindGuest(lookup, &g)) {
    t.Add(g.exact ? "  guest sub_%08X+0x%llX" : "  near guest sub_%08X (+0x%llX, approximate)",
          g.guest, static_cast<unsigned long long>(addr - g.host));
  } else if (lookup >= g_exe_base && lookup < g_exe_end) {
    t.Add("  (host code)");
  }
}

// ------------------------------------------------------------------------------------------------
// Guest state and host stack
// ------------------------------------------------------------------------------------------------

// On the crashing thread: the guest registers recompiled code last stored in its PPCContext.
__declspec(noinline) void CaptureGuestStateUnguarded(Request& r) {
  if (rex::runtime::ThreadState* ts = rex::runtime::ThreadState::Get()) {
    if (const PPCContext* c = ts->context()) {
      r.guest_tid = ts->thread_id();
      r.lr = c->lr;
      r.r1 = c->r1.u32;
      r.r3 = c->r3.u32;
      r.r4 = c->r4.u32;
      r.r5 = c->r5.u32;
      r.ctr = c->ctr.u32;
      r.last_indirect = c->last_indirect_target;
      r.has_guest = true;
    }
  }
}

void CaptureGuestState(Request& r) {
  r.has_guest = false;
  __try {
    CaptureGuestStateUnguarded(r);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    r.has_guest = false;
  }
}

__declspec(noinline) void GuestMemoryBasesUnguarded(uintptr_t* vbase, uintptr_t* pbase) {
  if (rex::Runtime* rt = rex::Runtime::instance()) {
    if (rex::memory::Memory* mem = rt->memory()) {
      *vbase = reinterpret_cast<uintptr_t>(mem->virtual_membase());
      *pbase = reinterpret_cast<uintptr_t>(mem->physical_membase());
    }
  }
}

void GuestMemoryBases(uintptr_t* vbase, uintptr_t* pbase) {
  *vbase = 0;
  *pbase = 0;
  __try {
    GuestMemoryBasesUnguarded(vbase, pbase);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

// x64 unwind from `start` with the image unwind data (recompiled functions have .pdata like any
// other), so every recompiled frame on the path is listed. Reads the crashing thread's stack,
// which stays frozen while that thread waits for the report. *n counts frames as they are found,
// so a fault part-way keeps the ones before it.
__declspec(noinline) void WalkStackUnguarded(const CONTEXT& start, ULONG_PTR low, ULONG_PTR high,
                                             uintptr_t* frames, int max, int* n) {
  g_walk_context = start;
  DWORD64 prev_rsp = 0;
  while (*n < max) {
    const DWORD64 pc = g_walk_context.Rip;
    if (!pc) break;
    frames[(*n)++] = static_cast<uintptr_t>(pc);
    DWORD64 image_base = 0;
    PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(pc, &image_base, nullptr);
    if (fe) {
      void* handler_data = nullptr;
      DWORD64 establisher = 0;
      RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, pc, fe, &g_walk_context, &handler_data,
                       &establisher, nullptr);
    } else {  // leaf function, or a jump to a bad address: the return address is at rsp
      if (!Readable(reinterpret_cast<const void*>(g_walk_context.Rsp), 8)) break;
      g_walk_context.Rip = *reinterpret_cast<const DWORD64*>(g_walk_context.Rsp);
      g_walk_context.Rsp += 8;
    }
    if (g_walk_context.Rsp <= prev_rsp) break;
    prev_rsp = g_walk_context.Rsp;
    if (low && high && (g_walk_context.Rsp < low || g_walk_context.Rsp >= high)) break;
  }
}

int WalkStack(const CONTEXT& start, ULONG_PTR low, ULONG_PTR high, uintptr_t* frames, int max) {
  int n = 0;
  __try {
    WalkStackUnguarded(start, low, high, frames, max, &n);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  return n;
}

// ------------------------------------------------------------------------------------------------
// Report text
// ------------------------------------------------------------------------------------------------

void FormatReport(Text& t, const SYSTEMTIME& st) {
  const Request& r = g_req;
  const EXCEPTION_RECORD* rec = r.ep ? r.ep->ExceptionRecord : nullptr;
  const CONTEXT* ctx = r.ep ? r.ep->ContextRecord : nullptr;
  char where[384];
  g_summary[0] = 0;

  switch (r.kind) {
    case Kind::kException:
      t.Add("===== cass crash guard: unhandled exception 0x%08lX (%s) =====\n",
            rec ? rec->ExceptionCode : 0ul, rec ? ExceptionName(rec->ExceptionCode) : "?");
      break;
    case Kind::kTerminate: t.Add("===== cass crash guard: std::terminate() called =====\n"); break;
    case Kind::kAbort: t.Add("===== cass crash guard: abort() called =====\n"); break;
    case Kind::kInvalidParameter:
      t.Add("===== cass crash guard: invalid parameter passed to a C runtime function =====\n");
      break;
    case Kind::kPureCall: t.Add("===== cass crash guard: pure virtual function call =====\n"); break;
  }
  t.Add("time %04u-%02u-%02u %02u:%02u:%02u.%03u, process %lu, thread %lu%s\n", st.wYear, st.wMonth,
        st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, GetCurrentProcessId(), r.tid,
        r.tid == g_install_tid ? " (the thread that installed the guard)" : "");
  if (g_exiting) {
    t.Add("the process was already exiting (ExitProcess had ended the other threads; this is a\n"
          "  failure in a DLL detach or static destructor), so this thread reported it itself\n");
  }
  if (r.detail[0]) t.Add("%s", r.detail);
  if (r.kind != Kind::kException) CopyLine(g_summary, sizeof(g_summary), r.summary, sizeof(r.summary));

  if (r.kind == Kind::kException && rec) {
    const auto addr = reinterpret_cast<uintptr_t>(rec->ExceptionAddress);
    DescribeLocation(addr, addr, where, sizeof(where));
    t.Add("faulting address: 0x%016llX = %s\n", static_cast<unsigned long long>(addr), where);
    _snprintf_s(g_summary, _TRUNCATE, "Unhandled exception 0x%08lX (%s) at %s.", rec->ExceptionCode,
                ExceptionName(rec->ExceptionCode), where);

    const DWORD code = rec->ExceptionCode;
    if ((code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR) && rec->NumberParameters >= 2) {
      const ULONG_PTR op = rec->ExceptionInformation[0];
      const auto target = static_cast<unsigned long long>(rec->ExceptionInformation[1]);
      const char* op_name = op == 0 ? "read" : op == 1 ? "write" : op == 8 ? "execute (DEP)" : "access";
      char guest[96] = {};
      uintptr_t vbase = 0, pbase = 0;
      GuestMemoryBases(&vbase, &pbase);
      if (target < 0x10000) {
        _snprintf_s(guest, _TRUNCATE, " (null pointer + 0x%llX)", target);
      } else if (vbase && target >= vbase && target - vbase < 0x100000000ull) {
        _snprintf_s(guest, _TRUNCATE, " = guest address 0x%08llX", target - vbase);
      } else if (pbase && target >= pbase && target - pbase < 0x100000000ull) {
        _snprintf_s(guest, _TRUNCATE, " = guest physical window +0x%08llX", target - pbase);
      }
      t.Add("access violation: %s of address 0x%016llX%s\n", op_name, target, guest);
      if (code == EXCEPTION_IN_PAGE_ERROR && rec->NumberParameters >= 3) {
        t.Add("in-page error status: 0x%08llX\n", static_cast<unsigned long long>(rec->ExceptionInformation[2]));
      }
      const size_t used = strnlen(g_summary, sizeof(g_summary));
      _snprintf_s(g_summary + used, sizeof(g_summary) - used, _TRUNCATE, " Bad %s of 0x%llX%s.", op_name,
                  target, guest);
    }
    if (code == EXCEPTION_ILLEGAL_INSTRUCTION && Readable(rec->ExceptionAddress, 2)) {
      const auto* b = static_cast<const uint8_t*>(rec->ExceptionAddress);
      if (b[0] == 0x0F && b[1] == 0x0B) {
        t.Add("the instruction is ud2: a __builtin_trap() in compiled code (in recompiled code, a\n"
              "  \"Switch case out of range\" default or another unreachable path)\n");
      }
    }
    if (code == EXCEPTION_STACK_OVERFLOW) {
      t.Add("stack overflow: runaway recursion, or a guest thread whose host stack is too small\n");
    }
    if (code == kCppExceptionCode) {
      char type[128], what[384];
      if (DescribeCppException(rec, type, sizeof(type), what, sizeof(what))) {
        t.Add("uncaught C++ exception %s: \"%s\"\n", type, what);
        _snprintf_s(g_summary, _TRUNCATE, "Uncaught C++ exception %s: %s", type, what);
      }
    }
    if (code == kStatusFailFast && rec->NumberParameters >= 1) {
      t.Add("fail-fast code %llu\n", static_cast<unsigned long long>(rec->ExceptionInformation[0]));
    }
  }

  if (r.has_guest) {
    t.Add("guest thread 0x%08X: lr=0x%08llX r1=0x%08X r3=0x%08X r4=0x%08X r5=0x%08X ctr=0x%08X\n"
          "  last indirect call target=0x%08X (values recompiled code last stored in PPCContext;\n"
          "  registers it keeps in locals can be stale)\n",
          r.guest_tid, static_cast<unsigned long long>(r.lr), r.r1, r.r3, r.r4, r.r5, r.ctr,
          r.last_indirect);
  } else {
    t.Add("no guest thread state on this thread (a host thread, or the runtime is not up yet)\n");
  }

  if (r.kind == Kind::kException && ctx) {
    t.Add("registers: rip=%016llX rsp=%016llX rbp=%016llX\n"
          "  rax=%016llX rbx=%016llX rcx=%016llX rdx=%016llX\n"
          "  rsi=%016llX rdi=%016llX r8=%016llX r9=%016llX\n"
          "  r10=%016llX r11=%016llX r12=%016llX r13=%016llX\n"
          "  r14=%016llX r15=%016llX\n",
          ctx->Rip, ctx->Rsp, ctx->Rbp, ctx->Rax, ctx->Rbx, ctx->Rcx, ctx->Rdx, ctx->Rsi, ctx->Rdi,
          ctx->R8, ctx->R9, ctx->R10, ctx->R11, ctx->R12, ctx->R13, ctx->R14, ctx->R15);
    if (Readable(reinterpret_cast<const void*>(ctx->Rip), 16)) {
      const auto* c = reinterpret_cast<const uint8_t*>(ctx->Rip);
      t.Add("code at rip: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\n",
            c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7], c[8], c[9], c[10], c[11], c[12], c[13],
            c[14], c[15]);
    } else {
      t.Add("rip is not readable (a call or jump to a bad address)\n");
    }
  }

  if (ctx) {
    uintptr_t frames[kMaxFrames];
    const int n = WalkStack(*ctx, r.stack_low, r.stack_high, frames, kMaxFrames);
    t.Add("call stack (%d frames; recompiled frames name their guest function):\n", n);
    for (int i = 0; i < n; ++i) {
      DescribeLocation(frames[i], i ? frames[i] - 1 : frames[i], where, sizeof(where));
      t.Add("  #%02d %s\n", i, where);
    }
  }
  if (!g_summary[0]) _snprintf_s(g_summary, _TRUNCATE, "The game hit a fatal error.");
}

// ------------------------------------------------------------------------------------------------
// Session log (through a helper thread, so a wedged logger cannot hang the report)
// ------------------------------------------------------------------------------------------------

bool LoggerExists() { return rex::GetLoggerRaw(rex::log::core()) != nullptr; }

void WriteLinesToLogger(const char* text, size_t len) {
  try {
    const char* p = text;
    const char* end = text + len;
    while (p < end) {
      const char* nl = static_cast<const char*>(memchr(p, '\n', static_cast<size_t>(end - p)));
      const char* e = nl ? nl : end;
      if (e > p) REXLOG_CRITICAL("[crash] {}", std::string_view(p, static_cast<size_t>(e - p)));
      if (!nl) break;
      p = nl + 1;
    }
    rex::FlushLogging();
  } catch (...) {
  }
}

bool LogLines(const char* text, size_t len) {
  // During process exit the logger may already be torn down (and the helper thread is gone).
  if (g_logger_stuck || g_exiting || !LoggerExists()) return false;
  // The helper itself failed (it is blocked in Begin): it cannot serve the request.
  if (g_req.tid == g_logger_tid.load()) return false;
  if (!g_logger_thread || !g_log_request || !g_log_done) {  // never started: log from here
    WriteLinesToLogger(text, len);
    return true;
  }
  if (!ThreadAlive(g_logger_thread)) return false;
  g_log_src = text;
  g_log_len = len;
  SetEvent(g_log_request);
  const HANDLE waits[2] = {g_log_done, g_logger_thread};
  if (WaitForMultipleObjects(2, waits, FALSE, kLoggerWaitMs) == WAIT_OBJECT_0) return true;
  g_logger_stuck = true;  // timed out, or the helper died
  return false;
}

DWORD WINAPI LoggerThread(void*) {
  for (;;) {
    if (WaitForSingleObject(g_log_request, INFINITE) != WAIT_OBJECT_0) return 0;
    WriteLinesToLogger(g_log_src, g_log_len);
    SetEvent(g_log_done);
  }
}

// ------------------------------------------------------------------------------------------------
// Minidump
// ------------------------------------------------------------------------------------------------

BOOL WriteDumpOnce(HANDLE file, MINIDUMP_TYPE type, MINIDUMP_EXCEPTION_INFORMATION* mei) {
  __try {
    return MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, type, mei, nullptr, nullptr);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    SetLastError(ERROR_NOACCESS);
    return FALSE;
  }
}

// Called on the reporter thread with the crashing thread's exception pointers. The guest's 4 GB
// reservation and its stacks make some flag sets fail (ERROR_NOACCESS), so smaller sets are tried
// in turn until one is written.
bool WriteDump(const wchar_t* path, MINIDUMP_TYPE* used, DWORD* error) {
  static const MINIDUMP_TYPE kTypes[] = {
      static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory |
                                 MiniDumpWithUnloadedModules | MiniDumpWithProcessThreadData),
      static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules),
      MiniDumpNormal,
      MiniDumpWithoutOptionalData,
  };
  MINIDUMP_EXCEPTION_INFORMATION mei{g_req.tid, g_req.ep, FALSE};
  *error = 0;
  for (const MINIDUMP_TYPE type : kTypes) {
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
      *error = GetLastError();
      return false;
    }
    const BOOL ok = WriteDumpOnce(f, type, g_req.ep ? &mei : nullptr);
    *error = ok ? 0 : GetLastError();
    CloseHandle(f);
    if (ok) {
      *used = type;
      return true;
    }
  }
  DeleteFileW(path);  // our own empty or partial file
  return false;
}

// Keeps the newest kKeepDumps crash dumps (they are megabytes each; the .txt reports stay).
void PruneOldDumps() {
  struct Item {
    FILETIME t;
    wchar_t name[MAX_PATH];
  };
  static Item items[128];
  int count = 0;
  wchar_t query[MAX_PATH];
  if (_snwprintf_s(query, _TRUNCATE, L"%ls\\cass_crash_*.dmp", g_logs_dir) < 0) return;
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW(query, &fd);
  if (h == INVALID_HANDLE_VALUE) return;
  do {
    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && count < 128 &&
        _snwprintf_s(items[count].name, _TRUNCATE, L"%ls\\%ls", g_logs_dir, fd.cFileName) >= 0) {
      items[count++].t = fd.ftLastWriteTime;
    }
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  while (count > kKeepDumps) {
    int oldest = 0;
    for (int i = 1; i < count; ++i) {
      if (CompareFileTime(&items[i].t, &items[oldest].t) < 0) oldest = i;
    }
    DeleteFileW(items[oldest].name);
    items[oldest] = items[--count];
  }
}

// ------------------------------------------------------------------------------------------------
// The report (reporter thread, or the crashing thread itself if the reporter never started)
// ------------------------------------------------------------------------------------------------

void ShowDialog(bool dump_ok, bool have_log) {
  // No window during process exit: the game window is gone, the other threads are dead and the
  // loader lock is held, so a MessageBox could block an exit that has no time cap.
  if (!g_dialog_enabled || g_exiting) return;
  wchar_t summary[600];
  ToWide(g_summary, summary, 600);
  wchar_t dump_line[MAX_PATH + 64];
  wchar_t log_line[MAX_PATH + 64];
  _snwprintf_s(dump_line, _TRUNCATE, dump_ok ? L"%ls" : L"(could not be written: %ls)", g_dmp_path);
  _snwprintf_s(log_line, _TRUNCATE, have_log ? L"%ls" : L"(no game log file for this session)", g_log_path);
  _snwprintf_s(g_message, _TRUNCATE,
               L"Captain America: Super Soldier has crashed.\n\n%ls\n\n"
               L"The details were saved to:\n\n"
               L"Crash report:\n%ls\n\nGame log:\n%ls\n\nCrash dump:\n%ls\n\n"
               L"Please include these files when you report the crash.",
               summary, g_txt_path, log_line, dump_line);
  MessageBoxW(nullptr, g_message, L"Captain America: Super Soldier - crash",
              MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND);
}

void ReportBody() {
  g_stage.store(kStageText);
  SYSTEMTIME st;
  GetLocalTime(&st);
  wchar_t stamp[40];
  _snwprintf_s(stamp, _TRUNCATE, L"%04u-%02u-%02u_%02u-%02u-%02u", st.wYear, st.wMonth, st.wDay,
               st.wHour, st.wMinute, st.wSecond);

  Text t{g_text, sizeof(g_text), 0};
  g_text[0] = 0;
  FormatReport(t, st);

  // <exe dir>\logs, else %TEMP% when the game folder is not writable.
  wchar_t dir[MAX_PATH];
  _snwprintf_s(dir, _TRUNCATE, L"%ls", g_logs_dir);
  CreateDirectoryW(dir, nullptr);
  _snwprintf_s(g_txt_path, _TRUNCATE, L"%ls\\cass_crash_%ls.txt", dir, stamp);
  if (!WriteFileAll(g_txt_path, "", 0, false)) {
    const DWORD n = GetTempPathW(MAX_PATH, dir);
    if (n && n < MAX_PATH) {
      if (dir[n - 1] == L'\\') dir[n - 1] = 0;
      _snwprintf_s(g_txt_path, _TRUNCATE, L"%ls\\cass_crash_%ls.txt", dir, stamp);
    }
  }
  _snwprintf_s(g_dmp_path, _TRUNCATE, L"%ls\\cass_crash_%ls.dmp", dir, stamp);

  char path8[MAX_PATH * 3];
  ToUtf8(g_txt_path, path8, sizeof(path8));
  t.Add("crash report: %s\n", path8);
  ToUtf8(g_dmp_path, path8, sizeof(path8));
  t.Add("minidump: %s (writing)\n", path8);
  g_text_len = t.len;
  WriteFileAll(g_txt_path, g_text, g_text_len, false);

  g_stage.store(kStageLog);
  const bool had_logger = !g_exiting && LoggerExists();
  const bool logged = LogLines(g_text, g_text_len);

  g_stage.store(kStageDump);
  MINIDUMP_TYPE used = MiniDumpNormal;
  DWORD error = 0;
  const bool dump_ok = WriteDump(g_dmp_path, &used, &error);
  char line[256];
  if (dump_ok) {
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    GetFileAttributesExW(g_dmp_path, GetFileExInfoStandard, &fa);
    const unsigned long long bytes = (static_cast<unsigned long long>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
    _snprintf_s(line, _TRUNCATE, "minidump written (type 0x%X, %llu KB)\n", static_cast<unsigned>(used),
                bytes / 1024);
  } else {
    _snprintf_s(line, _TRUNCATE, "minidump FAILED (error 0x%08lX)\n", error);
  }
  if (!logged) {
    const size_t used_len = strlen(line);
    _snprintf_s(line + used_len, sizeof(line) - used_len, _TRUNCATE,
                g_exiting    ? "not written to the session log (the process was exiting)\n"
                : had_logger ? "the session log did not accept the report (logger busy or wedged)\n"
                             : "no session log yet (the logger was not initialised)\n");
  }
  WriteFileAll(g_txt_path, line, strlen(line), true);
  LogLines(line, strlen(line));

  // The session log: the file LoggingConfig named, else the newest .log in the log folder.
  bool have_log = false;
  if (had_logger) {
    if (IsFile(g_session_log)) {
      have_log = _snwprintf_s(g_log_path, _TRUNCATE, L"%ls", g_session_log) >= 0;
    } else {
      have_log = (g_log_dir_cfg[0] && NewestFile(g_log_dir_cfg, L"*.log", g_log_path, MAX_PATH)) ||
                 NewestFile(g_logs_dir, L"*.log", g_log_path, MAX_PATH);
    }
  }
  if (!have_log) g_log_path[0] = 0;

  g_stage.store(kStageDialog);
  ShowDialog(dump_ok, have_log);
  g_stage.store(kStageDone);
  if (g_done) SetEvent(g_done);
}

// ------------------------------------------------------------------------------------------------
// Ownership: one report per process
// ------------------------------------------------------------------------------------------------

enum class Claim { kOwner, kAgain, kOther };

Claim ClaimReport() {
  const DWORD me = GetCurrentThreadId();
  DWORD expected = 0;
  // An idle reporter thread that fails becomes the owner too (Finish then reports inline).
  if (g_owner.compare_exchange_strong(expected, me)) return Claim::kOwner;
  // A second failure on the owner, or a fault inside the report on the reporter thread.
  if (expected == me || me == g_reporter_tid.load()) return Claim::kAgain;
  return Claim::kOther;
}

// True when this thread now owns the report and should fill g_req and call Finish. Otherwise the
// caller just continues its default fatal path: either this thread already reported (terminate ->
// abort -> SIGABRT reach here in turn), or another thread did and has finished (the wait keeps
// this thread from ending the process under the dump writer).
bool Begin(Kind kind) {
  switch (ClaimReport()) {
    case Claim::kOwner: break;
    case Claim::kAgain: return false;
    case Claim::kOther:
      if (g_done) WaitForSingleObject(g_done, INFINITE);
      return false;
  }
  g_req.kind = kind;
  g_req.tid = GetCurrentThreadId();
  g_req.ep = nullptr;
  g_req.tick = GetTickCount64();
  g_req.summary[0] = 0;
  g_req.detail[0] = 0;
  g_req.last_throw = t_last_throw;
  GetCurrentThreadStackLimits(&g_req.stack_low, &g_req.stack_high);
  CaptureGuestState(g_req);
  return true;
}

// Hands the report to the reporter thread and waits for it (dialog included). Without exception
// pointers (abort, terminate, ...) the current context is captured and presented as the fail-fast
// the process is about to die with, so the dump opens on this thread's stack.
__declspec(noinline) void Finish(EXCEPTION_POINTERS* ep, ULONG_PTR fail_fast_code) {
  if (!ep) {
    RtlCaptureContext(&g_synth_context);
    memset(&g_synth_record, 0, sizeof(g_synth_record));
    g_synth_record.ExceptionCode = kStatusFailFast;
    g_synth_record.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
    g_synth_record.ExceptionAddress = reinterpret_cast<void*>(g_synth_context.Rip);
    g_synth_record.NumberParameters = 1;
    g_synth_record.ExceptionInformation[0] = fail_fast_code;
    g_synth_pointers.ExceptionRecord = &g_synth_record;
    g_synth_pointers.ContextRecord = &g_synth_context;
    ep = &g_synth_pointers;
  }
  g_req.ep = ep;
  g_exiting = ProcessExiting();
  if (!g_exiting && GetCurrentThreadId() != g_reporter_tid.load() && ThreadAlive(g_reporter_thread) &&
      g_request && g_done) {
    SetEvent(g_request);
    const HANDLE waits[2] = {g_done, g_reporter_thread};
    const ULONGLONG start = GetTickCount64();
    for (;;) {
      const DWORD w = WaitForMultipleObjects(2, waits, FALSE, 1000);
      if (w != WAIT_TIMEOUT) break;  // reported, or the reporter thread is gone
      // A reporter stuck before the dialog (a lock this thread holds) must not hang the game forever.
      if (g_stage.load() < kStageDialog && GetTickCount64() - start > kOwnerWaitCapMs) break;
    }
  } else {
    // Report from this thread: the reporter could not be started, this is the reporter itself, or
    // the process is exiting and the reporter was ended with every other thread. (Waiting for it
    // then would hang the exit for kOwnerWaitCapMs and write nothing.)
    ReportBody();
  }
}

// "last C++ exception thrown on this thread ..." for the terminate/abort reports.
void AddLastThrow(Text& t, const Request& r) {
  if (!r.last_throw.count) {
    t.Add("no C++ exception was thrown on this thread before this\n");
    return;
  }
  t.Add("last C++ exception thrown on this thread, %llu ms earlier: %s: \"%s\"\n",
        static_cast<unsigned long long>(r.tick - r.last_throw.tick), r.last_throw.type, r.last_throw.what);
}

bool RecentThrow(const Request& r) { return r.last_throw.count && r.tick - r.last_throw.tick < 10000; }

// ------------------------------------------------------------------------------------------------
// The hooks
// ------------------------------------------------------------------------------------------------

LONG WINAPI OnUnhandledException(EXCEPTION_POINTERS* ep) {
  if (t_in_filter) return EXCEPTION_CONTINUE_SEARCH;  // a filter we chained to called us back
  t_in_filter = true;
  if (Begin(Kind::kException)) Finish(ep, 0);
  LONG result = EXCEPTION_EXECUTE_HANDLER;  // end the process quietly: the player has our dialog
  const LPTOP_LEVEL_EXCEPTION_FILTER prev = g_prev_filter.load();
  if (prev && prev != OnUnhandledException) {
    // Normally the C runtime's own filter, which turns an uncaught C++ exception into
    // std::terminate (ending in abort(), whose hooks see that this thread already reported).
    if (prev(ep) == EXCEPTION_CONTINUE_EXECUTION) result = EXCEPTION_CONTINUE_EXECUTION;
  }
  t_in_filter = false;
  return result;
}

[[noreturn]] void __cdecl OnTerminate() {
  if (Begin(Kind::kTerminate)) {
    Request& r = g_req;
    Text t{r.detail, sizeof(r.detail), 0};
    char type[128] = {}, what[384] = {};
    bool have = false;
    try {
      if (std::exception_ptr cur = std::current_exception()) {
        have = true;
        try {
          std::rethrow_exception(cur);
        } catch (const std::exception& e) {
          Undecorate(typeid(e).raw_name(), type, sizeof(type));
          CopyLine(what, sizeof(what), e.what(), sizeof(what) - 1);
        } catch (...) {
          CopyLine(type, sizeof(type), "(not derived from std::exception)", 64);
        }
      }
    } catch (...) {
    }
    if (have) {
      t.Add("uncaught C++ exception %s: \"%s\"\n", type, what);
      _snprintf_s(r.summary, _TRUNCATE, "Uncaught C++ exception %s: %s", type, what);
    } else {
      t.Add("no exception is active (std::terminate was called directly, or by a noexcept function)\n");
      if (RecentThrow(r)) {
        _snprintf_s(r.summary, _TRUNCATE, "std::terminate after C++ exception %s: %s", r.last_throw.type,
                    r.last_throw.what);
      } else {
        _snprintf_s(r.summary, _TRUNCATE, "std::terminate() was called.");
      }
    }
    AddLastThrow(t, r);
    Finish(nullptr, kFastFailFatalAppExit);
  }
  std::abort();
}

void __cdecl OnSigAbrt(int) {
  // raise() reset SIGABRT to SIG_DFL before calling us; put it back so a second thread aborting
  // meanwhile also waits for this report instead of fast-failing the process under it.
  signal(SIGABRT, OnSigAbrt);
  if (Begin(Kind::kAbort)) {
    Request& r = g_req;
    Text t{r.detail, sizeof(r.detail), 0};
    t.Add("abort() was called (REX_FATAL logs a [FATAL] line just before it; an uncaught C++\n"
          "  exception reaches it through std::terminate). Without this guard the process ends in\n"
          "  __fastfail(FAST_FAIL_FATAL_APP_EXIT): 0xC0000409 with data 7, which no exception\n"
          "  filter sees.\n");
    AddLastThrow(t, r);
    if (RecentThrow(r)) {
      _snprintf_s(r.summary, _TRUNCATE, "abort() after C++ exception %s: %s", r.last_throw.type,
                  r.last_throw.what);
    } else {
      _snprintf_s(r.summary, _TRUNCATE, "abort() was called (fatal error; see the game log).");
    }
    Finish(nullptr, kFastFailFatalAppExit);
    const _crt_signal_t prev = g_prev_sigabrt.load();
    if (prev) prev(SIGABRT);
  }
  // Returning lets abort() end the process with its __fastfail, as before.
}

void __cdecl OnInvalidParameter(const wchar_t* expression, const wchar_t* function, const wchar_t* file,
                                unsigned int line, uintptr_t) {
  if (Begin(Kind::kInvalidParameter)) {
    Request& r = g_req;
    Text t{r.detail, sizeof(r.detail), 0};
    if (expression || function || file) {
      char e[256], f[128], s[260];
      ToUtf8(expression, e, sizeof(e));
      ToUtf8(function, f, sizeof(f));
      ToUtf8(file, s, sizeof(s));
      t.Add("invalid parameter: %s in %s (%s:%u)\n", e, f, s, line);
    } else {
      t.Add("a C runtime function or STL check rejected a parameter (release builds give no detail;\n"
            "  the call stack shows the caller)\n");
    }
    AddLastThrow(t, r);
    _snprintf_s(r.summary, _TRUNCATE, "A C runtime function was passed an invalid parameter.");
    Finish(nullptr, kFastFailInvalidArg);
  }
  // The default (no handler) is _invoke_watson's fast fail; keep that, never continue with EINVAL.
  __fastfail(FAST_FAIL_INVALID_ARG);
}

void __cdecl OnPureCall() {
  if (Begin(Kind::kPureCall)) {
    Request& r = g_req;
    Text t{r.detail, sizeof(r.detail), 0};
    t.Add("pure virtual function call: an object was used after its destruction began, or before\n"
          "  its construction finished\n");
    _snprintf_s(r.summary, _TRUNCATE, "Pure virtual function call.");
    Finish(nullptr, kFastFailFatalAppExit);
  }
  // _purecall continues with abort().
}

// First chance, every thread, C++ throws only: remember type and what() for a later
// terminate/abort report, and make std::terminate on this thread reach OnTerminate. Appended last
// and always continues the search, so it changes nothing about how the exception is handled.
LONG CALLBACK RecordCppThrow(EXCEPTION_POINTERS* ep) {
  const EXCEPTION_RECORD* rec = ep->ExceptionRecord;
  if (rec->ExceptionCode != kCppExceptionCode) return EXCEPTION_CONTINUE_SEARCH;
  // Decoded into locals: a rethrow ("throw;") carries no ThrowInfo and must keep the record.
  char type[sizeof(ThrowRecord::type)], what[sizeof(ThrowRecord::what)];
  if (DescribeCppException(rec, type, sizeof(type), what, sizeof(what))) {
    ThrowRecord& r = t_last_throw;
    memcpy(r.type, type, sizeof(type));
    memcpy(r.what, what, sizeof(what));
    r.tick = GetTickCount64();
    ++r.count;
  }
  if (!t_terminate_hooked) {
    t_terminate_hooked = true;
    // Only replace the default (abort); leave a handler someone else set on this thread alone.
    const std::terminate_handler current = std::get_terminate();
    if (!current || current == &abort) std::set_terminate(OnTerminate);
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

// ------------------------------------------------------------------------------------------------
// Install, re-arm, reporter thread
// ------------------------------------------------------------------------------------------------

void ModuleNameOf(const void* p, char* out, int cap) {
  out[0] = 0;
  HMODULE mod = nullptr;
  if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         static_cast<LPCWSTR>(p), &mod) && mod) {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(mod, path, MAX_PATH - 1);
    const wchar_t* base = wcsrchr(path, L'\\');
    ToUtf8(base ? base + 1 : path, out, cap);
  }
  if (!out[0]) _snprintf_s(out, cap, _TRUNCATE, "%p", p);
}

// Another component may install its own unhandled-exception filter or SIGABRT handler after us
// (a driver, an overlay). Put ours back in front and chain to theirs after our report.
void RearmHandlers() {
  if (g_owner.load()) return;
  const LPTOP_LEVEL_EXCEPTION_FILTER prev = SetUnhandledExceptionFilter(OnUnhandledException);
  if (prev && prev != OnUnhandledException && prev != g_prev_filter.load()) {
    g_prev_filter.store(prev);
    char name[MAX_PATH * 3];
    ModuleNameOf(reinterpret_cast<const void*>(prev), name, sizeof(name));
    REXLOG_WARN("cass crash guard: the unhandled-exception filter was replaced by {}; re-installed ours "
                "(it chains to that one)",
                name);
  }
  const _crt_signal_t prev_sig = signal(SIGABRT, OnSigAbrt);
  if (prev_sig != OnSigAbrt && prev_sig != SIG_DFL && prev_sig != SIG_IGN && prev_sig != SIG_ERR &&
      prev_sig != g_prev_sigabrt.load()) {
    g_prev_sigabrt.store(prev_sig);
    REXLOG_WARN("cass crash guard: the SIGABRT handler was replaced; re-installed ours (it chains to that one)");
  }
}

DWORD WINAPI ReporterThread(void*) {
  for (;;) {
    const DWORD w = WaitForSingleObject(g_request, kRearmPeriodMs);
    if (w == WAIT_OBJECT_0) {
      ReportBody();
      Sleep(INFINITE);  // the process is ending; never report twice
    }
    if (w != WAIT_TIMEOUT) return 0;
    RearmHandlers();
  }
}

void NameThread(HANDLE thread, const wchar_t* name) {
  using SetDescription = HRESULT(WINAPI*)(HANDLE, PCWSTR);
  if (HMODULE k = GetModuleHandleW(L"kernel32.dll")) {
    if (auto fn = reinterpret_cast<SetDescription>(
            reinterpret_cast<void*>(GetProcAddress(k, "SetThreadDescription")))) {
      fn(thread, name);
    }
  }
}

void CaptureSessionLogPaths() {
  try {
    const rex::LogConfig& cfg = rex::LoggingConfig();
    std::error_code ec;
    if (!cfg.log_file.empty()) {
      const std::filesystem::path p = std::filesystem::absolute(cfg.log_file, ec);
      _snwprintf_s(g_session_log, _TRUNCATE, L"%ls", (ec ? cfg.log_file : p).c_str());
    }
    if (!cfg.log_dir.empty()) {
      const std::filesystem::path p = std::filesystem::absolute(cfg.log_dir, ec);
      _snwprintf_s(g_log_dir_cfg, _TRUNCATE, L"%ls", (ec ? cfg.log_dir : p).c_str());
    }
  } catch (...) {
  }
}

// ------------------------------------------------------------------------------------------------
// CASS_CRASH_TEST: crash on purpose to check the guard end to end
// ------------------------------------------------------------------------------------------------

char g_test_kind[32];
int g_test_delay_s = 10;

[[noreturn]] __declspec(noinline) void ThrowUnimplemented() {
  throw std::runtime_error("Unimplemented PPC instruction: trap (CASS_CRASH_TEST)");
}
void (*volatile g_thrower)() = ThrowUnimplemented;
__declspec(noinline) void ThrowThroughNoexcept() noexcept { g_thrower(); }

__declspec(noinline) int Recurse(int depth);
int (*volatile g_recurse)(int) = Recurse;
__declspec(noinline) int Recurse(int depth) {
  volatile char pad[4096];
  pad[depth & 4095] = static_cast<char>(depth);
  return g_recurse(depth + 1) + pad[0];
}

HANDLE g_test_go = nullptr;
DWORD WINAPI ConcurrentCrasher(void*) {
  WaitForSingleObject(g_test_go, INFINITE);
  *reinterpret_cast<volatile int*>(uintptr_t{16}) = 1;
  return 0;
}

void RunCrashTest(const char* kind) {
  REXLOG_WARN("cass crash guard: CASS_CRASH_TEST={} - crashing now", kind);
  if (!strcmp(kind, "av")) {
    *reinterpret_cast<volatile int*>(uintptr_t{8}) = 1;
  } else if (!strcmp(kind, "ud2")) {
#if defined(__clang__) || defined(__GNUC__)
    __builtin_trap();
#else
    __ud2();
#endif
  } else if (!strcmp(kind, "abort")) {
    std::abort();
  } else if (!strcmp(kind, "fatal")) {
    REX_FATAL("CASS_CRASH_TEST=fatal");
  } else if (!strcmp(kind, "terminate")) {
    std::terminate();
  } else if (!strcmp(kind, "throw")) {
    g_thrower();
  } else if (!strcmp(kind, "throw_noexcept")) {
    ThrowThroughNoexcept();
  } else if (!strcmp(kind, "invalid_parameter")) {
    _invalid_parameter_noinfo_noreturn();
  } else if (!strcmp(kind, "purecall")) {
    _purecall();
  } else if (!strcmp(kind, "stack_overflow")) {
    g_recurse(1);
  } else if (!strcmp(kind, "concurrent")) {
    g_test_go = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE a = CreateThread(nullptr, 0, ConcurrentCrasher, nullptr, 0, nullptr);
    HANDLE b = CreateThread(nullptr, 0, ConcurrentCrasher, nullptr, 0, nullptr);
    Sleep(100);
    SetEvent(g_test_go);
    if (a) WaitForSingleObject(a, INFINITE);
    if (b) WaitForSingleObject(b, INFINITE);
  } else {
    REXLOG_WARN("cass crash guard: unknown CASS_CRASH_TEST kind '{}'", kind);
  }
}

DWORD WINAPI CrashTestThread(void*) {
  Sleep(static_cast<DWORD>(g_test_delay_s) * 1000);
  RunCrashTest(g_test_kind);
  return 0;
}

void StartCrashTestIfRequested() {
  const DWORD n = GetEnvironmentVariableA("CASS_CRASH_TEST", g_test_kind, sizeof(g_test_kind));
  if (!n || n >= sizeof(g_test_kind)) return;
  char delay[16];
  const DWORD d = GetEnvironmentVariableA("CASS_CRASH_TEST_DELAY", delay, sizeof(delay));
  if (d && d < sizeof(delay)) g_test_delay_s = atoi(delay);
  if (g_test_delay_s < 0) g_test_delay_s = 0;
  REXLOG_WARN("cass crash guard: CASS_CRASH_TEST={} - crashing on purpose in {} s", g_test_kind, g_test_delay_s);
  if (HANDLE h = CreateThread(nullptr, 0, CrashTestThread, nullptr, 0, nullptr)) CloseHandle(h);
}

}  // namespace

namespace rex::glue {

void InstallCrashGuard() {
  static std::atomic<bool> s_installed{false};
  if (s_installed.exchange(true)) {
    // Again (e.g. OnPostInitLogging, then OnPreSetup): this thread's terminate handler, a fresh
    // look at the logging config, and our process-wide hooks back in front.
    if (std::get_terminate() != OnTerminate) std::set_terminate(OnTerminate);
    CaptureSessionLogPaths();
    RearmHandlers();
    return;
  }
  g_install_tid = GetCurrentThreadId();

  HMODULE self = GetModuleHandleW(nullptr);
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(self);
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const uint8_t*>(self) + dos->e_lfanew);
  g_exe_base = reinterpret_cast<uintptr_t>(self);
  g_exe_end = g_exe_base + nt->OptionalHeader.SizeOfImage;
  const IMAGE_DATA_DIRECTORY& pdata = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
  if (pdata.VirtualAddress && pdata.Size) {
    g_pdata = reinterpret_cast<const RUNTIME_FUNCTION*>(g_exe_base + pdata.VirtualAddress);
    g_pdata_count = pdata.Size / sizeof(RUNTIME_FUNCTION);
  }

  wchar_t exe[MAX_PATH] = {};
  const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
  if (n && n < MAX_PATH) {
    if (wchar_t* slash = wcsrchr(exe, L'\\')) *slash = 0;
    _snwprintf_s(g_logs_dir, _TRUNCATE, L"%ls\\logs", exe);
  } else {
    _snwprintf_s(g_logs_dir, _TRUNCATE, L"logs");
  }
  CreateDirectoryW(g_logs_dir, nullptr);
  CaptureSessionLogPaths();
  {
    char v[8];
    const DWORD len = GetEnvironmentVariableA("CASS_CRASH_DIALOG", v, sizeof(v));
    g_dialog_enabled = !(len && len < sizeof(v) && v[0] == '0');
  }
  (void)rex::log::core();  // register the category now, not inside a crash
  if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
    g_shutdown_in_progress = reinterpret_cast<ShutdownInProgressFn>(
        reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlDllShutdownInProgress")));
  }

  g_request = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  g_done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  g_log_request = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  g_log_done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  // Both handles are kept open (never closed): Finish and LogLines check the threads are alive.
  if (g_request && g_done) {
    DWORD tid = 0;
    if (HANDLE h = CreateThread(nullptr, 0, ReporterThread, nullptr, 0, &tid)) {
      g_reporter_thread = h;
      g_reporter_tid.store(tid);
      NameThread(h, L"cass crash guard");
    }
  }
  if (g_log_request && g_log_done) {
    DWORD tid = 0;
    if (HANDLE h = CreateThread(nullptr, 0, LoggerThread, nullptr, 0, &tid)) {
      g_logger_thread = h;
      g_logger_tid.store(tid);
      NameThread(h, L"cass crash guard log");
    }
  }

  std::set_terminate(OnTerminate);
  signal(SIGABRT, OnSigAbrt);
  _set_invalid_parameter_handler(OnInvalidParameter);
  _set_purecall_handler(OnPureCall);
  // Normally replaces the C runtime's filter (C++ exception -> std::terminate); we chain to it.
  g_prev_filter.store(SetUnhandledExceptionFilter(OnUnhandledException));
  const bool recorder = AddVectoredExceptionHandler(0, RecordCppThrow) != nullptr;
  PruneOldDumps();

  char logs8[MAX_PATH * 3];
  ToUtf8(g_logs_dir, logs8, sizeof(logs8));
  REXLOG_INFO("cass crash guard: installed (reports and minidumps -> {}, reporter thread {}, throw recorder {})",
              logs8, g_reporter_tid.load() ? "up" : "MISSING", recorder ? "on" : "off");
  StartCrashTestIfRequested();
}

}  // namespace rex::glue
