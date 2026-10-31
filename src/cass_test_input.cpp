// Adapted from santiquiroz/gears-of-war-2-pc-port src/gow2_test_input.cpp (BSD-3-Clause, Copyright (c) 2026 Santiago Quiroz Upegui and the gow2-recomp contributors).
//
// cass - scripted controller input for automated tests (format and behaviour: cass_test_input.h).
//
// From gow2 (src/gow2_test_input.cpp, src/gow2_test_input_hook.cpp): the button table, the token
// helpers and the big-endian XINPUT_STATE merge (OverlayPad), and the cvar-gated merge after the
// runtime's XamInputGetState. gow2 holds whatever a text file names; here a timed script runs on its
// own thread instead, presses wait for the game to read them, the physical pad is ignored while
// the script runs, and the script can take screenshots and end the process.

#include "cass_test_input.h"

#include <windows.h>
#include <objbase.h>  // CoInitializeEx / CoCreateInstance (the build defines WIN32_LEAN_AND_MEAN)
#include <wincodec.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>

// Screenshots: WIC's PNG encoder (ships with Windows).
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

#ifndef PW_CLIENTONLY
#define PW_CLIENTONLY 0x00000001
#endif
#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif

REXCVAR_DEFINE_STRING(cass_test_input, "", "Testing",
                      "Scripted controller input for automated tests: steps such as "
                      "\"wait 35; press start; wait 5; press a; quit\" (grammar in "
                      "src/cass_test_input.h), or @<file>. Drives guest user 0 without a controller "
                      "and without window focus. The environment variable CASS_TEST_INPUT wins. "
                      "Empty = off");

// ===============================================================================================
// Portable core: parsing and the XINPUT_STATE merge.
// ===============================================================================================

namespace rex::glue::test_input {
namespace {

struct ButtonName {
  std::string_view name;
  uint16_t bit;
};

// Accepted spellings (upper case). LT/RT are handled separately: they are triggers, not bits.
constexpr ButtonName kButtons[] = {
    {"UP", 0x0001},    {"DOWN", 0x0002},   {"LEFT", 0x0004}, {"RIGHT", 0x0008},
    {"START", 0x0010}, {"BACK", 0x0020},   {"SELECT", 0x0020}, {"LS", 0x0040},
    {"L3", 0x0040},    {"RS", 0x0080},     {"R3", 0x0080},   {"LB", 0x0100},
    {"RB", 0x0200},    {"A", 0x1000},      {"B", 0x2000},    {"X", 0x4000},
    {"Y", 0x8000},
};

// Display order and names.
constexpr ButtonName kCanonical[] = {
    {"A", 0x1000},  {"B", 0x2000},    {"X", 0x4000},    {"Y", 0x8000},  {"LB", 0x0100},
    {"RB", 0x0200}, {"START", 0x0010}, {"BACK", 0x0020}, {"LS", 0x0040}, {"RS", 0x0080},
    {"UP", 0x0001}, {"DOWN", 0x0002}, {"LEFT", 0x0004}, {"RIGHT", 0x0008},
};

constexpr uint8_t kFullTrigger = 255;
constexpr double kStickMax = 32767.0;
constexpr double kDefaultHoldSeconds = 0.15;
constexpr double kMinHoldSeconds = 0.02;
constexpr double kMaxHoldSeconds = 600.0;
constexpr double kMaxWaitSeconds = 3600.0;
constexpr size_t kMaxNameLength = 48;

std::string ToUpper(std::string_view text) {
  std::string out(text);
  for (char& c : out) c = char(std::toupper(static_cast<unsigned char>(c)));
  return out;
}

std::string ToLower(std::string_view text) {
  std::string out(text);
  for (char& c : out) c = char(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

bool IsSeparator(char c) { return c == ' ' || c == '\t' || c == ',' || c == '\r' || c == '\n'; }

std::string_view Trim(std::string_view text) {
  while (!text.empty() && IsSeparator(text.front())) text.remove_prefix(1);
  while (!text.empty() && IsSeparator(text.back())) text.remove_suffix(1);
  return text;
}

std::vector<std::string_view> SplitWords(std::string_view text) {
  std::vector<std::string_view> words;
  size_t i = 0;
  while (i < text.size()) {
    while (i < text.size() && IsSeparator(text[i])) ++i;
    const size_t start = i;
    while (i < text.size() && !IsSeparator(text[i])) ++i;
    if (i > start) words.push_back(text.substr(start, i - start));
  }
  return words;
}

// strtod on a copy: the token is not NUL-terminated, and trailing junk must be an error.
bool ParseNumber(std::string_view text, double* out) {
  const std::string copy(text);
  if (copy.empty()) return false;
  char* end = nullptr;
  const double value = std::strtod(copy.c_str(), &end);
  if (end != copy.c_str() + copy.size() || !std::isfinite(value)) return false;
  *out = value;
  return true;
}

bool ParseSeconds(std::string_view text, double min, double max, double* out, std::string* error) {
  double value = 0.0;
  if (!ParseNumber(text, &value)) {
    *error = fmt::format("'{}' is not a number of seconds", text);
    return false;
  }
  if (value < min || value > max) {
    *error = fmt::format("{} s is outside {}..{} s", value, min, max);
    return false;
  }
  *out = value;
  return true;
}

bool ParseButtons(std::string_view token, PadState* pad, std::string* error) {
  size_t start = 0;
  while (start <= token.size()) {
    const size_t plus = token.find('+', start);
    const std::string_view part =
        token.substr(start, plus == std::string_view::npos ? std::string_view::npos : plus - start);
    start = plus == std::string_view::npos ? token.size() + 1 : plus + 1;
    const std::string name = ToUpper(part);
    if (name.empty()) {
      *error = fmt::format("empty button name in '{}'", token);
      return false;
    }
    if (name == "LT") {
      pad->left_trigger = kFullTrigger;
      continue;
    }
    if (name == "RT") {
      pad->right_trigger = kFullTrigger;
      continue;
    }
    bool found = false;
    for (const ButtonName& b : kButtons) {
      if (b.name == name) {
        pad->buttons |= b.bit;
        found = true;
      }
    }
    if (!found) {
      *error = fmt::format(
          "unknown button '{}' (A B X Y START BACK LB RB LT RT LS RS UP DOWN LEFT RIGHT)", part);
      return false;
    }
  }
  return true;
}

std::string DescribeButtons(const PadState& pad) {
  std::string out;
  auto add = [&out](std::string_view name) {
    if (!out.empty()) out += '+';
    out += name;
  };
  for (const ButtonName& b : kCanonical) {
    if (pad.buttons & b.bit) add(b.name);
    if (b.bit == 0x0200) {  // triggers after the bumpers
      if (pad.left_trigger) add("LT");
      if (pad.right_trigger) add("RT");
    }
  }
  return out;
}

int16_t AxisValue(double v) { return int16_t(std::lround(std::clamp(v, -1.0, 1.0) * kStickMax)); }

std::string SanitizeName(std::string_view name) {
  std::string out;
  for (char c : name) {
    if (out.size() >= kMaxNameLength) break;
    const bool keep = std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-';
    out += keep ? c : '_';
  }
  return out;
}

bool ParseStep(std::string_view text, Step* step, std::string* error) {
  const std::vector<std::string_view> words = SplitWords(text);
  if (words.empty()) {
    *error = "empty step";
    return false;
  }
  const std::string verb = ToLower(words[0]);
  const size_t argc = words.size() - 1;

  if (verb == "log") {
    step->op = Op::kLog;
    step->text = std::string(Trim(text.substr(words[0].size())));
    return true;
  }
  if (verb == "wait") {
    step->op = Op::kWait;
    if (argc != 1) {
      *error = "usage: wait <seconds>";
      return false;
    }
    return ParseSeconds(words[1], 0.0, kMaxWaitSeconds, &step->seconds, error);
  }
  if (verb == "press" || verb == "hold") {
    step->op = Op::kPress;
    step->seconds = kDefaultHoldSeconds;
    std::string_view hold;
    if (verb == "press" && argc == 1) {
    } else if (verb == "press" && argc == 3 && ToLower(words[2]) == "hold") {
      hold = words[3];
    } else if (verb == "hold" && argc == 2) {
      hold = words[2];
    } else {
      *error = verb == "press" ? "usage: press <buttons> [hold <seconds>]"
                               : "usage: hold <buttons> <seconds>";
      return false;
    }
    if (!ParseButtons(words[1], &step->pad, error)) return false;
    if (!hold.empty() &&
        !ParseSeconds(hold, kMinHoldSeconds, kMaxHoldSeconds, &step->seconds, error)) {
      return false;
    }
    step->text = DescribeButtons(step->pad);
    return true;
  }
  if (verb == "stick") {
    step->op = Op::kStick;
    if (argc != 4) {
      *error = "usage: stick <left|right> <x -1..1> <y -1..1> <seconds>";
      return false;
    }
    const std::string side = ToLower(words[1]);
    if (side != "left" && side != "right") {
      *error = fmt::format("stick '{}': expected left or right", words[1]);
      return false;
    }
    double x = 0.0, y = 0.0;
    if (!ParseNumber(words[2], &x) || !ParseNumber(words[3], &y) || x < -1.0 || x > 1.0 ||
        y < -1.0 || y > 1.0) {
      *error = fmt::format("stick position '{} {}': expected two numbers in -1..1", words[2], words[3]);
      return false;
    }
    if (!ParseSeconds(words[4], kMinHoldSeconds, kMaxHoldSeconds, &step->seconds, error)) {
      return false;
    }
    if (side == "left") {
      step->pad.lx = AxisValue(x);
      step->pad.ly = AxisValue(y);
    } else {
      step->pad.rx = AxisValue(x);
      step->pad.ry = AxisValue(y);
    }
    step->text = fmt::format("{} {:.2f} {:.2f}", side, x, y);
    return true;
  }
  if (verb == "screenshot") {
    step->op = Op::kScreenshot;
    if (argc > 1) {
      *error = "usage: screenshot [name]";
      return false;
    }
    step->text = argc == 1 ? SanitizeName(words[1]) : std::string();
    return true;
  }
  if (verb == "quit") {
    step->op = Op::kQuit;
    if (argc != 0) {
      *error = "usage: quit";
      return false;
    }
    return true;
  }
  *error = fmt::format("unknown command '{}' (wait, press, hold, stick, log, screenshot, quit)",
                       words[0]);
  return false;
}

uint16_t LoadBe16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }

uint32_t LoadBe32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

void StoreBe16(uint8_t* p, uint16_t v) {
  p[0] = uint8_t(v >> 8);
  p[1] = uint8_t(v);
}

void StoreBe32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24);
  p[1] = uint8_t(v >> 16);
  p[2] = uint8_t(v >> 8);
  p[3] = uint8_t(v);
}

void OverlayAxis(uint8_t* p, int16_t value) {
  if (value != 0) StoreBe16(p, uint16_t(value));
}

}  // namespace

ParsedScript ParseScript(std::string_view script) {
  ParsedScript out;
  int step_no = 0;
  size_t line_start = 0;
  while (line_start <= script.size()) {
    const size_t nl = script.find('\n', line_start);
    std::string_view line = script.substr(
        line_start, nl == std::string_view::npos ? std::string_view::npos : nl - line_start);
    line_start = nl == std::string_view::npos ? script.size() + 1 : nl + 1;
    if (const size_t hash = line.find('#'); hash != std::string_view::npos) line = line.substr(0, hash);

    size_t seg_start = 0;
    while (seg_start <= line.size()) {
      const size_t semi = line.find(';', seg_start);
      const std::string_view seg = Trim(line.substr(
          seg_start, semi == std::string_view::npos ? std::string_view::npos : semi - seg_start));
      seg_start = semi == std::string_view::npos ? line.size() + 1 : semi + 1;
      if (seg.empty()) continue;
      ++step_no;
      std::string error;
      Step step;
      if (!out.steps.empty() && out.steps.back().op == Op::kQuit) {
        error = "quit must be the last step";
      } else if (ParseStep(seg, &step, &error)) {
        out.steps.push_back(std::move(step));
        continue;
      }
      out.error = fmt::format("step {} '{}': {}", step_no, seg, error);
      out.steps.clear();
      return out;
    }
  }
  if (out.steps.empty()) out.error = "the script has no steps";
  return out;
}

std::string DescribeStep(const Step& step) {
  switch (step.op) {
    case Op::kWait:
      return fmt::format("wait {:.2f}s", step.seconds);
    case Op::kPress:
      return fmt::format("press {} (hold {:.2f}s)", step.text, step.seconds);
    case Op::kStick:
      return fmt::format("stick {} ({:.2f}s)", step.text, step.seconds);
    case Op::kLog:
      return fmt::format("log: {}", step.text);
    case Op::kScreenshot:
      return step.text.empty() ? std::string("screenshot") : fmt::format("screenshot {}", step.text);
    case Op::kQuit:
      return "quit";
  }
  return "?";
}

void OverlayPad(uint8_t* state, const PadState& pad, uint32_t packet_bump, uint32_t* result) {
  if (*result != 0) {
    std::memset(state, 0, 16);
    *result = 0;
  }
  StoreBe32(state, LoadBe32(state) + packet_bump);
  StoreBe16(state + 4, uint16_t(LoadBe16(state + 4) | pad.buttons));
  state[6] = std::max(state[6], pad.left_trigger);
  state[7] = std::max(state[7], pad.right_trigger);
  OverlayAxis(state + 8, pad.lx);
  OverlayAxis(state + 10, pad.ly);
  OverlayAxis(state + 12, pad.rx);
  OverlayAxis(state + 14, pad.ry);
}

}  // namespace rex::glue::test_input

// ===============================================================================================
// Runtime side: the shared pad state, the script thread, screenshots and quit.
// ===============================================================================================

namespace rex::glue {
namespace {

using test_input::Op;
using test_input::PadState;
using test_input::Step;
using Clock = std::chrono::steady_clock;

constexpr double kReleaseGapSeconds = 0.25;   // minimum released time between two presses
constexpr uint64_t kReadsPerEdge = 2;         // pad reads that must see a press / a release
constexpr double kEdgeTimeoutSeconds = 10.0;  // give up waiting for those reads after this
constexpr double kFirstReadTimeoutSeconds = 300.0;
constexpr double kReadPollSeconds = 0.005;
constexpr DWORD kCaptureTimeoutMs = 5000;
constexpr int kExitTimeoutSeconds = 30;
constexpr UINT kForcedExitCode = 3;
constexpr size_t kMaxScriptFileBytes = 1 << 20;

std::atomic<bool> g_active{false};
// While the script runs, user 0's physical pad is ignored so a person touching the controller (or
// stick drift) cannot derail a test; it passes through again after a script without quit ends.
std::atomic<bool> g_exclusive{false};
std::atomic<bool> g_logged_physical{false};
// The scripted state, written by the script thread, read by every guest pad read of user 0.
// g_pad_word = buttons | lt << 16 | rt << 24 | change serial << 32 (added to packet_number);
// g_stick_word = lx | ly << 16 | rx << 32 | ry << 48. A read between the two stores sees a mix of
// two consecutive states for one poll, which is harmless.
std::atomic<uint64_t> g_pad_word{0};
std::atomic<uint64_t> g_stick_word{0};
std::atomic<uint32_t> g_serial{0};
std::atomic<uint64_t> g_reads{0};  // user-0 pad reads since install

std::atomic<bool> g_stop{false};
std::atomic<bool> g_process_exiting{false};  // trivially destructible: safe during static teardown
std::atomic<bool> g_runner_done{false};      // set as the script thread's last action
std::mutex g_wait_lock;
std::condition_variable g_wake;
std::mutex g_thread_lock;
std::thread g_runner;  // joined or detached by StopRunner before its destructor can run

HWND g_window = nullptr;  // set once at install
Clock::time_point g_install_time;
std::wstring g_stamp;  // launch time, names this run's screenshots

double SecondsSince(Clock::time_point t) {
  return std::chrono::duration<double>(Clock::now() - t).count();
}

void SetPad(const PadState& pad) {
  const uint32_t serial = g_serial.fetch_add(1, std::memory_order_relaxed) + 1;
  g_stick_word.store(uint64_t(uint16_t(pad.lx)) | (uint64_t(uint16_t(pad.ly)) << 16) |
                         (uint64_t(uint16_t(pad.rx)) << 32) | (uint64_t(uint16_t(pad.ry)) << 48),
                     std::memory_order_release);
  g_pad_word.store(uint64_t(pad.buttons) | (uint64_t(pad.left_trigger) << 16) |
                       (uint64_t(pad.right_trigger) << 24) | (uint64_t(serial) << 32),
                   std::memory_order_release);
}

// false when ShutdownTestInput asked the script to stop.
bool WaitSeconds(double seconds) {
  std::unique_lock<std::mutex> hold(g_wait_lock);
  return !g_wake.wait_for(hold, std::chrono::duration<double>(seconds),
                          [] { return g_stop.load(std::memory_order_acquire); });
}

enum class ReadWait { kOk, kTimeout, kStopped };

ReadWait WaitForReads(uint64_t target, double timeout_seconds) {
  const Clock::time_point start = Clock::now();
  while (g_reads.load(std::memory_order_acquire) < target) {
    if (SecondsSince(start) >= timeout_seconds) return ReadWait::kTimeout;
    if (!WaitSeconds(kReadPollSeconds)) return ReadWait::kStopped;
  }
  return ReadWait::kOk;
}

// Holds pad, waits `seconds` and until the game has read it, releases, and waits the release gap
// and until the game has read the release. false when stopped.
bool HoldPad(const PadState& pad, double seconds, const std::string& what) {
  SetPad(pad);
  const uint64_t pressed_at = g_reads.load(std::memory_order_acquire);
  bool ok = WaitSeconds(seconds);
  if (ok) {
    const ReadWait r = WaitForReads(pressed_at + kReadsPerEdge, kEdgeTimeoutSeconds);
    if (r == ReadWait::kStopped) ok = false;
    if (r == ReadWait::kTimeout) {
      REXLOG_WARN("cass test input: the game did not read the pad within {:.0f}s while {} was held; "
                  "releasing anyway",
                  kEdgeTimeoutSeconds, what);
    }
  }
  SetPad(PadState{});
  if (!ok) return false;
  const uint64_t released_at = g_reads.load(std::memory_order_acquire);
  if (!WaitSeconds(kReleaseGapSeconds)) return false;
  const ReadWait r = WaitForReads(released_at + kReadsPerEdge, kEdgeTimeoutSeconds);
  if (r == ReadWait::kTimeout) {
    REXLOG_WARN("cass test input: the game did not read the pad within {:.0f}s after {} was released",
                kEdgeTimeoutSeconds, what);
  }
  return r != ReadWait::kStopped;
}

// --- text helpers --------------------------------------------------------------------------------

std::string WideToUtf8(std::wstring_view text) {
  if (text.empty()) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0, nullptr,
                                    nullptr);
  std::string out(size_t(std::max(n, 0)), '\0');
  if (n > 0) {
    WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), out.data(), n, nullptr, nullptr);
  }
  return out;
}

std::wstring Utf8ToWide(std::string_view text) {
  if (text.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0);
  std::wstring out(size_t(std::max(n, 0)), L'\0');
  if (n > 0) MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), out.data(), n);
  return out;
}

std::string PathUtf8(const std::filesystem::path& p) { return WideToUtf8(p.native()); }

std::string TrimCopy(std::string_view text) {
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
  return std::string(text);
}

// One line for the log: new lines become "; ", long scripts are cut.
std::string OneLine(std::string_view script) {
  std::string out;
  for (char c : script) {
    if (c == '\r') continue;
    if (c == '\n') {
      out += "; ";
    } else {
      out += c;
    }
    if (out.size() > 400) {
      out += " ...";
      break;
    }
  }
  return out;
}

bool GetEnvUtf8(const wchar_t* name, std::string* out) {
  const DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
  if (n == 0) return false;
  std::wstring value(n, L'\0');
  const DWORD got = GetEnvironmentVariableW(name, value.data(), n);
  if (got == 0 || got >= n) return false;
  value.resize(got);
  *out = WideToUtf8(value);
  return true;
}

// Off: returns false. On: *script is the text to parse, *origin where it came from; *error is set
// when an @file could not be read.
bool ReadScriptSetting(std::string* script, std::string* origin, std::string* error) {
  std::string value;
  if (GetEnvUtf8(L"CASS_TEST_INPUT", &value) && !TrimCopy(value).empty()) {
    *origin = "CASS_TEST_INPUT";
  } else {
    value = REXCVAR_GET(cass_test_input);
    *origin = "cass_test_input";
  }
  value = TrimCopy(value);
  if (value.empty()) return false;
  if (value.front() != '@') {
    *script = std::move(value);
    return true;
  }
  std::error_code ec;
  std::filesystem::path path(Utf8ToWide(TrimCopy(std::string_view(value).substr(1))));
  const std::filesystem::path full = std::filesystem::absolute(path, ec);
  if (!ec) path = full;
  *origin += " file " + PathUtf8(path);
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    *error = "cannot open the script file " + PathUtf8(path);
    return true;
  }
  std::string text;
  text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  if (text.size() > kMaxScriptFileBytes) {
    *error = "the script file is larger than 1 MB";
    return true;
  }
  if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
      static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) {
    text.erase(0, 3);  // UTF-8 BOM
  }
  *script = std::move(text);
  return true;
}

// --- game window ---------------------------------------------------------------------------------

struct WindowSearch {
  HWND best = nullptr;
  long long area = -1;
};

BOOL CALLBACK FindProcessWindow(HWND hwnd, LPARAM param) {
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (pid != GetCurrentProcessId() || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER)) {
    return TRUE;
  }
  RECT rc{};
  GetClientRect(hwnd, &rc);
  const long long area = (long long)(rc.right - rc.left) * (rc.bottom - rc.top);
  auto* search = reinterpret_cast<WindowSearch*>(param);
  if (area > search->area) {
    search->area = area;
    search->best = hwnd;
  }
  return TRUE;
}

// The window CassApp handed over, else the largest visible top-level window of this process.
HWND GameWindow() {
  if (g_window && IsWindow(g_window)) return g_window;
  WindowSearch search;
  EnumWindows(FindProcessWindow, reinterpret_cast<LPARAM>(&search));
  return search.best;
}

// --- screenshots ---------------------------------------------------------------------------------

struct Capture {
  int width = 0;
  int height = 0;
  std::vector<uint8_t> bgr;  // top-down rows of width * 3 bytes
  const char* method = "";
  bool black = true;
};

bool LooksBlack(const uint8_t* bgra, int width, int height) {
  const size_t count = size_t(width) * size_t(height);
  for (size_t i = 0; i < count; i += 61) {
    const uint8_t* p = bgra + i * 4;
    if (p[0] > 8 || p[1] > 8 || p[2] > 8) return false;
  }
  return true;
}

// PrintWindow with PW_RENDERFULLCONTENT reads the window's DWM surface, so it works behind other
// windows; a screen copy of the client area is the fallback when that comes back black.
bool CaptureWindow(HWND hwnd, Capture* out) {
  RECT rc{};
  if (!GetClientRect(hwnd, &rc)) return false;
  const int w = rc.right - rc.left;
  const int h = rc.bottom - rc.top;
  if (w <= 0 || h <= 0) return false;
  HDC window_dc = GetDC(hwnd);
  if (!window_dc) return false;
  HDC mem = CreateCompatibleDC(window_dc);
  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = w;
  bi.bmiHeader.biHeight = -h;  // top-down
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HBITMAP bmp = mem ? CreateDIBSection(window_dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
  bool ok = false;
  if (bmp && bits) {
    HGDIOBJ old = SelectObject(mem, bmp);
    ok = PrintWindow(hwnd, mem, PW_CLIENTONLY | PW_RENDERFULLCONTENT) != FALSE;
    GdiFlush();
    out->method = "PrintWindow";
    if (!ok || LooksBlack(static_cast<const uint8_t*>(bits), w, h)) {
      POINT origin{0, 0};
      ClientToScreen(hwnd, &origin);
      HDC screen = GetDC(nullptr);
      if (screen && BitBlt(mem, 0, 0, w, h, screen, origin.x, origin.y, SRCCOPY | CAPTUREBLT)) {
        ok = true;
        out->method = "screen copy";
      }
      if (screen) ReleaseDC(nullptr, screen);
      GdiFlush();
    }
    if (ok) {
      const uint8_t* src = static_cast<const uint8_t*>(bits);
      out->width = w;
      out->height = h;
      out->black = LooksBlack(src, w, h);
      out->bgr.resize(size_t(w) * size_t(h) * 3);
      uint8_t* dst = out->bgr.data();
      for (size_t i = 0, n = size_t(w) * size_t(h); i < n; ++i) {
        dst[i * 3 + 0] = src[i * 4 + 0];
        dst[i * 3 + 1] = src[i * 4 + 1];
        dst[i * 3 + 2] = src[i * 4 + 2];
      }
    }
    SelectObject(mem, old);
  }
  if (bmp) DeleteObject(bmp);
  if (mem) DeleteDC(mem);
  ReleaseDC(hwnd, window_dc);
  return ok;
}

// PrintWindow can block on the window's thread; never let that stall the script.
bool CaptureWithTimeout(HWND hwnd, Capture* out, bool* timed_out) {
  struct Job {
    std::mutex lock;
    std::condition_variable done_cv;
    bool done = false;
    bool ok = false;
    Capture capture;
  };
  auto job = std::make_shared<Job>();
  std::thread([job, hwnd] {
    Capture capture;
    const bool ok = CaptureWindow(hwnd, &capture);
    std::lock_guard<std::mutex> hold(job->lock);
    job->capture = std::move(capture);
    job->ok = ok;
    job->done = true;
    job->done_cv.notify_all();
  }).detach();
  std::unique_lock<std::mutex> hold(job->lock);
  *timed_out = !job->done_cv.wait_for(hold, std::chrono::milliseconds(kCaptureTimeoutMs),
                                      [&job] { return job->done; });
  if (*timed_out || !job->ok) return false;
  *out = std::move(job->capture);
  return true;
}

template <typename T>
void ReleaseCom(T*& p) {
  if (p) p->Release();
  p = nullptr;
}

bool SavePng(const std::filesystem::path& path, const Capture& cap) {
  const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  IWICImagingFactory* factory = nullptr;
  IWICStream* stream = nullptr;
  IWICBitmapEncoder* encoder = nullptr;
  IWICBitmapFrameEncode* frame = nullptr;
  IPropertyBag2* props = nullptr;
  HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory));
  if (SUCCEEDED(hr)) hr = factory->CreateStream(&stream);
  if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
  if (SUCCEEDED(hr)) hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
  if (SUCCEEDED(hr)) hr = encoder->Initialize(stream, WICBitmapEncoderNoCache);
  if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(&frame, &props);
  if (SUCCEEDED(hr)) hr = frame->Initialize(props);
  if (SUCCEEDED(hr)) hr = frame->SetSize(UINT(cap.width), UINT(cap.height));
  WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
  if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&format);
  if (SUCCEEDED(hr) && !IsEqualGUID(format, GUID_WICPixelFormat24bppBGR)) hr = E_FAIL;
  const UINT stride = UINT(cap.width) * 3;
  if (SUCCEEDED(hr)) {
    hr = frame->WritePixels(UINT(cap.height), stride, stride * UINT(cap.height),
                            const_cast<BYTE*>(cap.bgr.data()));
  }
  if (SUCCEEDED(hr)) hr = frame->Commit();
  if (SUCCEEDED(hr)) hr = encoder->Commit();
  ReleaseCom(props);
  ReleaseCom(frame);
  ReleaseCom(encoder);
  ReleaseCom(stream);
  ReleaseCom(factory);
  if (SUCCEEDED(init)) CoUninitialize();
  if (FAILED(hr)) DeleteFileW(path.c_str());
  return SUCCEEDED(hr);
}

bool SaveBmp(const std::filesystem::path& path, const Capture& cap) {
  const uint32_t row = (uint32_t(cap.width) * 3 + 3) & ~3u;
  BITMAPFILEHEADER fh{};
  BITMAPINFOHEADER ih{};
  fh.bfType = 0x4D42;  // "BM"
  fh.bfOffBits = sizeof(fh) + sizeof(ih);
  fh.bfSize = fh.bfOffBits + row * uint32_t(cap.height);
  ih.biSize = sizeof(ih);
  ih.biWidth = cap.width;
  ih.biHeight = cap.height;  // bottom-up
  ih.biPlanes = 1;
  ih.biBitCount = 24;
  ih.biCompression = BI_RGB;
  std::ofstream f(path, std::ios::binary);
  if (!f) return false;
  f.write(reinterpret_cast<const char*>(&fh), sizeof(fh));
  f.write(reinterpret_cast<const char*>(&ih), sizeof(ih));
  std::vector<char> line(row, 0);
  for (int y = cap.height - 1; y >= 0; --y) {
    std::memcpy(line.data(), cap.bgr.data() + size_t(y) * size_t(cap.width) * 3, size_t(cap.width) * 3);
    f.write(line.data(), std::streamsize(row));
  }
  return bool(f);
}

std::filesystem::path LogDir() {
  std::error_code ec;
  try {
    const rex::LogConfig& cfg = rex::LoggingConfig();
    if (!cfg.log_dir.empty()) {
      const std::filesystem::path p = std::filesystem::absolute(cfg.log_dir, ec);
      return ec ? cfg.log_dir : p;
    }
  } catch (...) {
  }
  wchar_t exe[MAX_PATH] = {};
  const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return L"logs";
  return std::filesystem::path(exe).parent_path() / L"logs";
}

void TakeScreenshot(const std::string& name, const std::string& where) {
  static int counter = 0;
  ++counter;
  const std::string label = name.empty() ? std::string("screenshot") : "screenshot " + name;
  HWND hwnd = GameWindow();
  if (!hwnd) {
    REXLOG_WARN("cass test input: {} {}: no game window found", label, where);
    return;
  }
  Capture cap;
  bool timed_out = false;
  if (!CaptureWithTimeout(hwnd, &cap, &timed_out)) {
    REXLOG_WARN("cass test input: {} {}: {}", label, where,
                timed_out ? "capture did not finish within 5 s" : "capture failed");
    return;
  }
  std::error_code ec;
  const std::filesystem::path dir = LogDir();
  std::filesystem::create_directories(dir, ec);
  wchar_t number[16] = {};
  swprintf_s(number, L"_%02d", counter);
  std::wstring file = L"cass_test_" + g_stamp + number;
  if (!name.empty()) file += L"_" + Utf8ToWide(name);
  std::filesystem::path path = dir / (file + L".png");
  const char* note = cap.black ? ", image is black: window minimized?" : "";
  if (SavePng(path, cap)) {
    REXLOG_INFO("cass test input: {} {} -> {} ({}x{}, {}{})", label, where, PathUtf8(path), cap.width,
                cap.height, cap.method, note);
    return;
  }
  path.replace_extension(L".bmp");
  if (SaveBmp(path, cap)) {
    REXLOG_INFO("cass test input: {} {} -> {} ({}x{}, {}{}; PNG encoder failed, saved as BMP)", label,
                where, PathUtf8(path), cap.width, cap.height, cap.method, note);
    return;
  }
  REXLOG_WARN("cass test input: {} {}: could not write {}", label, where, PathUtf8(path));
}

// --- quit ----------------------------------------------------------------------------------------

// Asks the script thread to stop and joins it. bounded: give it a few seconds (a screenshot can
// take up to kCaptureTimeoutMs) and detach it if it is still busy.
void StopRunner(bool bounded) {
  std::thread runner;
  {
    std::lock_guard<std::mutex> hold(g_thread_lock);
    runner = std::move(g_runner);
  }
  if (!runner.joinable()) return;
  {
    std::lock_guard<std::mutex> hold(g_wait_lock);
    g_stop.store(true, std::memory_order_release);
  }
  g_wake.notify_all();
  if (runner.get_id() == std::this_thread::get_id()) {
    runner.detach();
    return;
  }
  if (bounded) {
    for (int i = 0; i < 700 && !g_runner_done.load(std::memory_order_acquire); ++i) Sleep(10);
    if (!g_runner_done.load(std::memory_order_acquire)) {
      runner.detach();
      return;
    }
  }
  runner.join();
}

// atexit: an exit() that does not come through CassApp::OnShutdown (the SDK's window close uses
// std::_Exit and runs no atexit handler at all) must not reach the destructor of a joinable
// g_runner, which would call std::terminate and make the crash guard report a crash.
void OnProcessExit() {
  g_process_exiting.store(true, std::memory_order_release);
  StopRunner(/*bounded=*/true);
}

[[noreturn]] void ForceExit() {
  rex::FlushLogging();
  TerminateProcess(GetCurrentProcess(), kForcedExitCode);
  for (;;) Sleep(INFINITE);
}

// A hung shutdown (TerminateTitle not returning) must still end the test. Only atomics and Win32
// calls once the process is in its exit path (static destructors may already have run); the log
// line only before that.
void StartExitWatchdog() {
  std::thread([] {
    for (int i = 0; i < kExitTimeoutSeconds * 10; ++i) Sleep(100);
    if (!g_process_exiting.load(std::memory_order_acquire)) {
      REXLOG_ERROR("cass test input: the game did not exit within {} s of the close request; "
                   "terminating the process (exit code {})",
                   kExitTimeoutSeconds, kForcedExitCode);
      ForceExit();
    }
    TerminateProcess(GetCurrentProcess(), kForcedExitCode);
  }).detach();
}

// Same path as the window's close button: WM_CLOSE -> SDL close request -> ReXApp::OnClosing,
// which terminates the title, flushes the log and calls std::_Exit(0) (exit code 0; OnShutdown,
// atexit handlers and static destructors do not run on that path).
void RequestQuit() {
  rex::FlushLogging();
  HWND hwnd = GameWindow();
  if (!hwnd || !PostMessageW(hwnd, WM_CLOSE, 0, 0)) {
    REXLOG_ERROR("cass test input: no game window to close (error {}); terminating the process "
                 "(exit code {})",
                 GetLastError(), kForcedExitCode);
    ForceExit();
  }
  StartExitWatchdog();
}

// --- the script ----------------------------------------------------------------------------------

struct Script {
  std::vector<Step> steps;
  std::string error;
};

void RunScript(std::shared_ptr<const Script> script) {
  if (!script->error.empty()) {
    REXLOG_ERROR("cass test input: ERROR {} - nothing was run; closing the game", script->error);
    REXLOG_INFO("cass test input: done");
    RequestQuit();
    return;
  }

  // t = 0 is the game's first read of user 0's pad, not the launch.
  const Clock::time_point wait_start = Clock::now();
  switch (WaitForReads(1, kFirstReadTimeoutSeconds)) {
    case ReadWait::kStopped:
      REXLOG_INFO("cass test input: stopped before the game read the pad");
      return;
    case ReadWait::kTimeout:
      REXLOG_WARN("cass test input: the game has not read the pad {:.0f}s after launch; running the "
                  "script anyway",
                  SecondsSince(g_install_time));
      break;
    case ReadWait::kOk:
      REXLOG_INFO("cass test input: the game reads the pad {:.2f}s after launch (waited {:.2f}s); "
                  "script starts, t=0",
                  SecondsSince(g_install_time), SecondsSince(wait_start));
      break;
  }

  const Clock::time_point start = Clock::now();
  const size_t n = script->steps.size();
  for (size_t i = 0; i < n; ++i) {
    const Step& step = script->steps[i];
    const std::string where = fmt::format("[step {}/{}, t={:.2f}s]", i + 1, n, SecondsSince(start));
    bool ok = true;
    switch (step.op) {
      case Op::kWait:
        REXLOG_INFO("cass test input: {} {}", test_input::DescribeStep(step), where);
        ok = WaitSeconds(step.seconds);
        break;
      case Op::kPress:
      case Op::kStick:
        REXLOG_INFO("cass test input: {} {}", test_input::DescribeStep(step), where);
        ok = HoldPad(step.pad, step.seconds,
                     step.op == Op::kPress ? step.text : "stick " + step.text);
        break;
      case Op::kLog:
        REXLOG_INFO("cass test input: log: {} {}", step.text, where);
        break;
      case Op::kScreenshot:
        TakeScreenshot(step.text, where);
        break;
      case Op::kQuit:
        REXLOG_INFO("cass test input: quit {} - closing the game window", where);
        REXLOG_INFO("cass test input: done");
        RequestQuit();
        return;
    }
    if (!ok || g_stop.load(std::memory_order_acquire)) {
      REXLOG_INFO("cass test input: stopped at step {}/{} (the game is closing)", i + 1, n);
      return;
    }
  }
  g_exclusive.store(false, std::memory_order_release);
  REXLOG_INFO("cass test input: done (t={:.2f}s; no quit step, the game keeps running; the "
              "physical controller works again)",
              SecondsSince(start));
}

void RunnerMain(std::shared_ptr<const Script> script) {
  RunScript(std::move(script));
  g_runner_done.store(true, std::memory_order_release);
}

std::wstring LaunchStamp() {
  const std::time_t now = std::time(nullptr);
  std::tm local{};
  localtime_s(&local, &now);
  wchar_t buf[32] = {};
  wcsftime(buf, 32, L"%Y%m%d-%H%M%S", &local);
  return buf;
}

}  // namespace

bool TestInputActive() { return g_active.load(std::memory_order_acquire); }

void ApplyTestInput(uint8_t* base, uint32_t user_index, uint32_t state_addr, uint32_t* result) {
  if (user_index != 0 || state_addr == 0 || !g_active.load(std::memory_order_acquire)) return;
  uint8_t* state = rex::memory::GuestPtr(base, state_addr);
  if (*result == 0 && g_exclusive.load(std::memory_order_acquire)) {
    // Drop the physical state; OverlayPad then writes a connected pad holding only the script's.
    const bool pressed = state[4] != 0 || state[5] != 0 || state[6] > 30 || state[7] > 30;
    if (pressed && !g_logged_physical.exchange(true, std::memory_order_acq_rel)) {
      REXLOG_WARN("cass test input: the physical controller on user 0 is ignored while the script "
                  "runs (do not touch it during a test)");
    }
    *result = 1;
  }
  const uint64_t pad_word = g_pad_word.load(std::memory_order_acquire);
  const uint64_t stick_word = g_stick_word.load(std::memory_order_acquire);
  PadState pad;
  pad.buttons = uint16_t(pad_word);
  pad.left_trigger = uint8_t(pad_word >> 16);
  pad.right_trigger = uint8_t(pad_word >> 24);
  pad.lx = int16_t(uint16_t(stick_word));
  pad.ly = int16_t(uint16_t(stick_word >> 16));
  pad.rx = int16_t(uint16_t(stick_word >> 32));
  pad.ry = int16_t(uint16_t(stick_word >> 48));
  test_input::OverlayPad(state, pad, uint32_t(pad_word >> 32), result);
  g_reads.fetch_add(1, std::memory_order_acq_rel);
}

void InstallTestInput(void* native_window) {
  static std::atomic<bool> installed{false};
  if (installed.exchange(true)) return;

  std::string text, origin, read_error;
  if (!ReadScriptSetting(&text, &origin, &read_error)) return;  // off: nothing at all

  g_window = static_cast<HWND>(native_window);
  g_install_time = Clock::now();
  g_stamp = LaunchStamp();
  std::atexit(OnProcessExit);  // registered after g_runner was constructed: runs before its destructor

  auto script = std::make_shared<Script>();
  if (!read_error.empty()) {
    script->error = read_error;
  } else {
    test_input::ParsedScript parsed = test_input::ParseScript(text);
    script->steps = std::move(parsed.steps);
    script->error = std::move(parsed.error);
  }
  if (script->error.empty()) {
    REXLOG_INFO("cass test input: on, {} step(s) from {}: {}", script->steps.size(), origin,
                OneLine(text));
    REXLOG_INFO("cass test input: drives guest user 0 through XamInputGetState (no controller or "
                "window focus needed); waiting for the game to read the pad");
    SetPad(PadState{});
    g_exclusive.store(true, std::memory_order_release);
    g_active.store(true, std::memory_order_release);
  } else {
    REXLOG_ERROR("cass test input: script from {} rejected", origin);
  }

  std::lock_guard<std::mutex> hold(g_thread_lock);
  g_runner = std::thread(RunnerMain, std::shared_ptr<const Script>(std::move(script)));
}

void ShutdownTestInput() { StopRunner(/*bounded=*/false); }

}  // namespace rex::glue
