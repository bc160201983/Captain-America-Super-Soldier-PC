// Adapted from santiquiroz/gears-of-war-2-pc-port src/gow2_crash_watch.h (BSD-3-Clause, Copyright (c) 2026 Santiago Quiroz Upegui and the gow2-recomp contributors).
//
// cass - crash guard: every fatal error leaves a log line, a crash report, a minidump and a
// message to the player.
//
// Why: a recompiled-code failure used to end the process with no trace at all. REX_UNIMPLEMENTED
// throws std::runtime_error, nothing catches it, std::terminate calls abort(), and abort() ends in
// __fastfail(FAST_FAIL_FATAL_APP_EXIT): Windows reports 0xC0000409 with data 7 in ucrtbase.dll,
// no exception filter ever runs, and cass_NNN.log stops mid-game with no fatal line.
//
// What is caught, and how (all of it is last-chance; nothing here changes an error that something
// else handles):
//   * abort() from anywhere (REX_FATAL, std::terminate's default, the C runtime): a SIGABRT handler,
//     process-wide. abort() raises SIGABRT before its __fastfail, so this is the only hook that
//     sees the 0xC0000409/7 crash.
//   * std::terminate(): a terminate handler. In the UCRT that handler is stored per thread
//     (ucrt/misc/terminate.cpp keeps it in the per-thread data), so it is set on the installing
//     thread and, lazily, on every thread that throws a C++ exception (see the throw recorder).
//   * hardware exceptions and C++ exceptions nothing handles (access violation, the ud2 of a
//     __builtin_trap, stack overflow, an uncaught throw): SetUnhandledExceptionFilter. The filter is
//     only called once every vectored and frame-based handler has declined, so the runtime's own
//     guest-memory handlers, guest __try/__except scopes and CassFpExceptionHandler keep first
//     claim. The reporter thread re-installs it every 2 s if another component replaced it, and
//     then chains to that component's filter.
//   * invalid parameters passed to the C runtime / STL checks, and pure virtual calls: their CRT
//     handlers. Both still end the process exactly as before (fast fail / abort).
//   * the type and what() of the last C++ exception thrown on each thread: a vectored handler,
//     appended LAST and always returning EXCEPTION_CONTINUE_SEARCH, copies them at throw time.
//     That is what names the exception when terminate/abort runs after the stack has unwound.
//
// What a crash produces (first crash only; other threads that fail meanwhile wait for it):
//   * CRITICAL lines "[crash] ..." in the session log (logs/cass_NNN.log), then a logger flush;
//   * <exe dir>/logs/cass_crash_<YYYY-MM-DD_HH-MM-SS>.txt - the same report, written with plain
//     Win32 calls before the logger is touched, so it exists even when the logger is wedged;
//   * <exe dir>/logs/cass_crash_<stamp>.dmp - a minidump (thread info, indirectly referenced
//     memory, unloaded modules; falls back to smaller sets when the guest arena makes one fail);
//     the newest 10 dumps are kept;
//   * one MessageBox naming those files.
// The report names the exception, the faulting address as module+offset, the read/write address
// of an access violation (also as a guest address when it falls in the guest arena), the guest
// state of the thread (lr, r1, r3-r5, ctr, last indirect call target), the registers, and the host
// call stack with every recompiled frame resolved to its guest sub_XXXXXXXX via PPCFuncMappings.
//
// The report itself runs on a reporter thread created at install time (so a stack overflow or a
// loader-lock holder can still be reported), and the logger is driven from a third thread with a
// timeout (so a crash inside a logger call cannot hang the report). During process exit
// (ExitProcess has already ended those threads; an abort() or fault in a DLL's static destructor)
// the failing thread writes the .txt report and the minidump itself, with no log lines and no
// dialog, and the process ends at once instead of waiting for a reporter that no longer exists.
//
// Environment switches:
//   CASS_CRASH_DIALOG=0          no MessageBox (test rigs, unattended runs)
//   CASS_CRASH_TEST=<kind>       crash on purpose to check the guard: av, ud2, abort, fatal,
//                                terminate, throw, throw_noexcept, invalid_parameter, purecall,
//                                stack_overflow, concurrent
//   CASS_CRASH_TEST_DELAY=<s>    seconds after install before the test crash (default 10)

#pragma once

namespace rex::glue {

// Installs everything above. Idempotent: a second call only re-applies the per-thread terminate
// handler on the calling thread, re-reads the session log path and re-installs the process-wide
// hooks. Safe before the logger exists (it then logs nothing and the report still goes to the
// .txt file); call it as early as possible - CassApp::OnPostInitLogging, and again first thing in
// CassApp::OnPreSetup.
void InstallCrashGuard();

}  // namespace rex::glue
