// Adapted from santiquiroz/gears-of-war-2-pc-port src/gow2_test_input.h (BSD-3-Clause, Copyright (c) 2026 Santiago Quiroz Upegui and the gow2-recomp contributors).
//
// cass - scripted controller input for automated tests.
//
// A script drives guest user 0's pad with no physical controller and no window focus, so a test
// rig can start cass.exe, walk the menus and end the process by itself. It is merged into what the
// guest's XamInputGetState returns (src/cass_test_input_hook.cpp), after the runtime's own call, so
// the SDK's "ignore input while the window is unfocused" rule does not apply to it. While the
// script runs, user 0's physical pad is ignored (logged once if it is used), so a person touching the
// controller cannot derail a test; after a script without "quit" ends it works again, merged with
// the idle virtual pad.
//
// Off unless the environment variable CASS_TEST_INPUT is set (else the cvar cass_test_input, e.g.
// --cass_test_input="..."). Unset or empty: no thread, no log line, and the XamInputGetState
// override only forwards to the runtime.
//
// The value is the script itself, or "@<path>" to read it from a file (relative to the current
// directory). Grammar, case-insensitive; steps are separated by ';' or new lines, '#' starts a
// comment that runs to the end of the line:
//
//   wait <seconds>                     do nothing for that long
//   press <buttons> [hold <seconds>]   hold the buttons (default 0.15 s), then release them;
//   hold <buttons> <seconds>           same as press ... hold <seconds>
//   stick <left|right> <x> <y> <secs>  hold a stick (x, y in -1..1; y = 1 is up), then centre it
//   log <text>                         write "cass test input: log: <text>" to the session log
//   screenshot [name]                  save the game window as <log dir>/cass_test_<stamp>_<nn>_<name>.png
//   quit                               log "cass test input: done", close the game window (as the
//                                      close button does) and let the game shut down; must be last
//
//   <buttons> = one or more of A B X Y START BACK LB RB LT RT LS RS UP DOWN LEFT RIGHT joined
//   with '+' (SELECT = BACK, L3/R3 = LS/RS; UP/DOWN/LEFT/RIGHT are the d-pad).
//
// Timing: the clock starts when the game first reads user 0's pad (logged with the time since
// launch), so "wait" values do not include window/D3D12 setup. A press is only released after the
// game has read the pressed state at least twice, and the next step only starts after the game has
// read the released state at least twice (each capped at 10 s, logged when it runs out), so a press
// is never lost to a frame hitch. Each step is logged as "cass test input: <step> [step i/n, t=..s]".
//
// Ending the test: "quit" posts WM_CLOSE to the game window; the SDK's close path (ReXApp::OnClosing)
// terminates the title, flushes the log and calls std::_Exit(0), so a clean quit exits with code 0.
// If the process is still alive 30 s later, it logs that, flushes the log and terminates itself
// with exit code 3 (the shutdown hung; not a test failure by itself). A script that does
// not parse is logged ("cass test input: ERROR ...") and quits the same way, so a typo ends the run
// instead of leaving it waiting. A script without "quit" logs "done" and leaves the game running.
//
// Example (cold start to loading save slot 1; the game shows "Press the START button" about 40 s
// after its first pad read, and SELECT SAVE SLOT 5-10 s after START, with slot 1 highlighted):
//   set CASS_TEST_INPUT=wait 45; press start; wait 15; screenshot slots; press a; wait 60; quit

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace rex::glue {

// Reads CASS_TEST_INPUT / cass_test_input and, when set, parses the script and starts the script
// thread. native_window is the game window's HWND (window()->GetNativeWindowHandle(); may be null,
// the window is then looked up by process). Call once the window exists and before the guest runs:
// CassApp::OnPreLaunchModule. Idempotent.
void InstallTestInput(void* native_window);

// Stops the script thread and waits for it. Call from CassApp::OnShutdown (reached when the guest
// main thread ends or the launch fails; closing the window hard-exits without it). An atexit
// handler does the same, bounded, for any other exit(). No-op when off.
void ShutdownTestInput();

// True once InstallTestInput accepted a script. Cheap; read on every guest pad read.
bool TestInputActive();

// Called after the runtime's XamInputGetState (src/cass_test_input_hook.cpp). result is its return
// value (0 = connected). For user 0 a failed read becomes a connected pad, so the game never sees
// the virtual pad unplug between presses; the scripted state is written into the 16-byte
// big-endian XINPUT_STATE at state_addr (replacing the physical state while the script runs,
// merged with it afterwards). Other users are untouched.
void ApplyTestInput(uint8_t* base, uint32_t user_index, uint32_t state_addr, uint32_t* result);

}  // namespace rex::glue

namespace rex::glue::test_input {

struct PadState {
  uint16_t buttons = 0;
  uint8_t left_trigger = 0;
  uint8_t right_trigger = 0;
  int16_t lx = 0, ly = 0, rx = 0, ry = 0;
  bool operator==(const PadState&) const = default;
};

enum class Op { kWait, kPress, kStick, kLog, kScreenshot, kQuit };

struct Step {
  Op op = Op::kWait;
  double seconds = 0.0;  // wait length, press hold time, stick hold time
  PadState pad;          // press / stick: the state held
  std::string text;      // log text, screenshot name, or the button/stick description
};

struct ParsedScript {
  std::vector<Step> steps;
  std::string error;  // empty when the whole script parsed
};

ParsedScript ParseScript(std::string_view script);

// "wait 2.00s", "press A+LB (hold 0.15s)", "stick left 0.00 1.00 (2.00s)", ...
std::string DescribeStep(const Step& step);

// state: a 16-byte big-endian XINPUT_STATE in host memory. A failed read (result != 0) becomes a
// connected pad that holds only the scripted state; otherwise buttons OR, triggers max, and a
// non-zero scripted stick axis wins.
void OverlayPad(uint8_t* state, const PadState& pad, uint32_t packet_bump, uint32_t* result);

}  // namespace rex::glue::test_input
