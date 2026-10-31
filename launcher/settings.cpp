// Adapted from OverkillLabs3/gears-judgement-recomp launcher/settings.cpp (BSD-3-Clause, Copyright (c) 2026 the contributors to this project).
//
// Captain America: Super Soldier (PC) - launcher and settings.
//
// A small native window for the settings people change most: game folder, display mode,
// internal resolution, vsync and frame limits, SMAA / FSR, texture filtering, FPS overlay and a
// few advanced switches. It edits cass.toml next to this program (the same file cass.exe and its
// F4 settings overlay use), changes only the keys it owns, keeps every other line and comment as
// it is (including CRLF/LF line ends), and can start the game.
//
// Only components that ship with Windows are used (user32, gdi32, GDI+, dwmapi, shell32, ole32,
// comctl32, dxgi) and the C runtime is linked statically, so there is nothing to install.
//
//   Captain America Launcher.exe [--config <path>]                          opens the window
//   Captain America Launcher.exe [--config <path>] --apply key=value ...    writes without a window
//
// --apply understands any cass.toml key ("value" "-" comments the key out) plus these shortcuts:
//   resolution=original|1296p|1440p|3456x1944|4k   texture_filtering=default|4x|8x|16x
//   render_path=auto|rov|rtv                       game_dir=<folder with default.xex, or the .iso>
// Exit code: 0 written, 1 could not write, 2 bad argument, 3 game_dir is not a game folder or
// game .iso (nothing written for 2 and 3).

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <dxgi.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "default_config.h"
#include "../src/cass_xdvdfs.h"  // ISO check shared with cass.exe

using namespace Gdiplus;

namespace {

// ------------------------------------------------------------------------------------------
// config file model: a list of lines (each with its own line end), edited in place
// ------------------------------------------------------------------------------------------
constexpr const char* kLauncherSection = "# ---------- Launcher ----------";

struct Line {
  std::string text;
  std::string eol;  // "\r\n", "\n", or "" for a last line without a line end
};

std::wstring g_dir;       // folder of this program (and of cass.exe)
std::wstring g_cfg_path;  // <dir>\cass.toml unless --config says otherwise
std::vector<Line> g_lines;
std::string g_eol = "\r\n";  // line end used for new lines: whatever the file mostly uses

std::string Narrow(const std::wstring& w) {
  if (w.empty()) return "";
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
  return s;
}

std::wstring Widen(const std::string& s) {
  if (s.empty()) return L"";
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
  return w;
}

std::vector<Line> SplitLines(const std::string& text, std::string* eol) {
  std::vector<Line> out;
  size_t crlf = 0, lf = 0, i = 0;
  while (i < text.size()) {
    size_t j = text.find('\n', i);
    if (j == std::string::npos) {
      out.push_back({text.substr(i), ""});
      break;
    }
    if (j > i && text[j - 1] == '\r') {
      out.push_back({text.substr(i, j - 1 - i), "\r\n"});
      ++crlf;
    } else {
      out.push_back({text.substr(i, j - i), "\n"});
      ++lf;
    }
    i = j + 1;
  }
  if (eol) *eol = (crlf || lf) ? (crlf > lf ? "\r\n" : "\n") : "\r\n";
  return out;
}

std::string JoinLines() {
  std::string s;
  for (const auto& l : g_lines) s += l.text + l.eol;
  return s;
}

bool ReadTextFile(const std::wstring& path, std::string& out) {
  HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER sz;
  GetFileSizeEx(f, &sz);
  out.resize(size_t(sz.QuadPart));
  DWORD got = 0;
  const bool ok = out.empty() || (ReadFile(f, out.data(), DWORD(out.size()), &got, nullptr) && got == out.size());
  CloseHandle(f);
  return ok;
}

bool WriteTextFile(const std::wstring& path, const std::string& text) {
  HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return false;
  DWORD put = 0;
  bool ok = text.empty() || (WriteFile(f, text.data(), DWORD(text.size()), &put, nullptr) && put == text.size());
  ok = FlushFileBuffers(f) && ok;
  CloseHandle(f);
  return ok;
}

// Temp file next to the target, then one rename: the game never sees a half-written file.
bool WriteFileAtomic(const std::wstring& path, const std::string& text) {
  const std::wstring tmp = path + L".launcher.tmp";
  if (!WriteTextFile(tmp, text)) {
    DeleteFileW(tmp.c_str());
    return false;
  }
  if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    DeleteFileW(tmp.c_str());
    return false;
  }
  return true;
}

std::string g_loaded_text;  // what LoadConfig read: an unchanged file is not rewritten

void LoadConfig() {
  std::string text;
  if (!ReadTextFile(g_cfg_path, text)) {
    text = kDefaultConfig;
    g_loaded_text.clear();  // file missing: always write
  } else {
    g_loaded_text = text;
  }
  g_lines = SplitLines(text, &g_eol);
}

bool SaveConfig() {
  const std::string text = JoinLines();
  if (!g_loaded_text.empty() && text == g_loaded_text) return true;
  if (!WriteFileAtomic(g_cfg_path, text)) return false;
  g_loaded_text = text;
  return true;
}

// --- TOML values -----------------------------------------------------------------------------
bool IsWs(char c) { return c == ' ' || c == '\t'; }

// Parses one TOML value starting at s[pos] (whitespace first is skipped). On success sets the
// decoded value, [*vbeg, *vend) as the value's span in s, and requires only whitespace or a
// comment after it.
bool ParseValue(const std::string& s, size_t pos, std::string* value, size_t* vbeg, size_t* vend) {
  while (pos < s.size() && IsWs(s[pos])) ++pos;
  if (pos >= s.size()) return false;
  std::string v;
  size_t e;
  if (s[pos] == '\'') {
    e = s.find('\'', pos + 1);
    if (e == std::string::npos) return false;
    v = s.substr(pos + 1, e - pos - 1);
    ++e;
  } else if (s[pos] == '"') {
    e = pos + 1;
    bool closed = false;
    while (e < s.size()) {
      const char c = s[e];
      if (c == '"') { closed = true; ++e; break; }
      if (c == '\\' && e + 1 < s.size()) {
        const char n = s[e + 1];
        e += 2;
        switch (n) {
          case 'n': v += '\n'; break;
          case 't': v += '\t'; break;
          case 'r': v += '\r'; break;
          case '"': v += '"'; break;
          case '\\': v += '\\'; break;
          case 'u': case 'U': {
            const size_t len = n == 'u' ? 4 : 8;
            if (e + len > s.size()) return false;
            const unsigned long cp = strtoul(s.substr(e, len).c_str(), nullptr, 16);
            e += len;
            std::wstring w;
            if (cp >= 0x10000) {
              w += wchar_t(0xD800 + ((cp - 0x10000) >> 10));
              w += wchar_t(0xDC00 + ((cp - 0x10000) & 0x3FF));
            } else {
              w += wchar_t(cp);
            }
            v += Narrow(w);
          } break;
          default: v += n; break;
        }
        continue;
      }
      v += c;
      ++e;
    }
    if (!closed) return false;
  } else {
    e = pos;
    while (e < s.size() && !IsWs(s[e]) && s[e] != '#') ++e;
    v = s.substr(pos, e - pos);
    if (v.empty()) return false;
    // a bare value is a number, a boolean, or something simple like an inf/nan; never a list of words
    for (char c : v)
      if (c == ',' || c == '"' || c == '\'' || c == '=' || c == '[' || c == '{') return false;
  }
  size_t t = e;
  while (t < s.size() && IsWs(s[t])) ++t;
  if (t < s.size() && s[t] != '#') return false;
  *value = v;
  *vbeg = pos;
  *vend = e;
  return true;
}

// Does line `l` assign `key`? `commented`: the "# key = value" form. *vbeg/*vend: value span.
bool MatchKey(const std::string& l, const std::string& key, bool commented, std::string* value, size_t* vbeg,
              size_t* vend, size_t* hash = nullptr) {
  size_t b = 0;
  while (b < l.size() && IsWs(l[b])) ++b;
  if (b >= l.size()) return false;
  if (commented) {
    if (l[b] != '#') return false;
    if (hash) *hash = b;
    ++b;
    while (b < l.size() && IsWs(l[b])) ++b;
  } else if (l[b] == '#') {
    return false;
  }
  if (l.compare(b, key.size(), key) != 0) return false;
  size_t e = b + key.size();
  if (e < l.size() && !IsWs(l[e]) && l[e] != '=') return false;  // a longer key with this prefix
  while (e < l.size() && IsWs(l[e])) ++e;
  if (e >= l.size() || l[e] != '=') return false;
  std::string v;
  size_t vb, ve;
  if (!ParseValue(l, e + 1, &v, &vb, &ve)) return false;
  if (value) *value = v;
  if (vbeg) *vbeg = vb;
  if (vend) *vend = ve;
  return true;
}

int FindActive(const std::string& key) {
  for (size_t i = 0; i < g_lines.size(); ++i)
    if (MatchKey(g_lines[i].text, key, false, nullptr, nullptr, nullptr)) return int(i);
  return -1;
}

int FindCommented(const std::string& key) {
  for (size_t i = 0; i < g_lines.size(); ++i)
    if (MatchKey(g_lines[i].text, key, true, nullptr, nullptr, nullptr)) return int(i);
  return -1;
}

bool GetValue(const std::string& key, std::string* value) {
  const int i = FindActive(key);
  if (i < 0) return false;
  size_t vb, ve;
  return MatchKey(g_lines[i].text, key, false, value, &vb, &ve);
}

void InsertInLauncherSection(const std::string& text) {
  int header = -1;
  for (size_t i = 0; i < g_lines.size(); ++i) {
    std::string t = g_lines[i].text;
    while (!t.empty() && IsWs(t.back())) t.pop_back();
    if (t == kLauncherSection) { header = int(i); break; }
  }
  if (header < 0) {
    if (!g_lines.empty() && g_lines.back().eol.empty()) g_lines.back().eol = g_eol;
    if (!g_lines.empty() && !g_lines.back().text.empty()) g_lines.push_back({"", g_eol});
    g_lines.push_back({kLauncherSection, g_eol});
    g_lines.push_back({"# Written by Captain America Launcher.exe.", g_eol});
    g_lines.push_back({text, g_eol});
    return;
  }
  size_t pos = size_t(header) + 1;
  for (size_t k = size_t(header) + 1; k < g_lines.size(); ++k) {
    if (g_lines[k].text.rfind("# ----------", 0) == 0) break;
    bool blank = true;
    for (char c : g_lines[k].text) blank = blank && IsWs(c);
    if (!blank) pos = k + 1;
  }
  if (g_lines[pos - 1].eol.empty()) g_lines[pos - 1].eol = g_eol;
  g_lines.insert(g_lines.begin() + pos, Line{text, g_eol});
}

// Sets `key` to the TOML text `toml`. An active line keeps its spacing and trailing comment; a
// "# key = value" line is uncommented; otherwise the key goes into the Launcher section.
void SetValue(const std::string& key, const std::string& toml) {
  std::string v;
  size_t vb, ve, hash;
  int i = FindActive(key);
  if (i >= 0 && MatchKey(g_lines[i].text, key, false, &v, &vb, &ve)) {
    std::string& l = g_lines[i].text;
    l = l.substr(0, vb) + toml + l.substr(ve);
    return;
  }
  i = FindCommented(key);
  if (i >= 0 && MatchKey(g_lines[i].text, key, true, &v, &vb, &ve, &hash)) {
    std::string& l = g_lines[i].text;
    size_t after = hash + 1;
    while (after < l.size() && IsWs(l[after])) ++after;
    const std::string line = l.substr(0, vb) + toml + l.substr(ve);
    l = line.substr(0, hash) + line.substr(after);
    return;
  }
  InsertInLauncherSection(key + " = " + toml);
}

// Comments the active line out ("# key = value"), so the runtime's built-in default applies and
// the old value stays visible.
void CommentOut(const std::string& key) {
  const int i = FindActive(key);
  if (i < 0) return;
  std::string& l = g_lines[i].text;
  size_t b = 0;
  while (b < l.size() && IsWs(l[b])) ++b;
  l.insert(b, "# ");
}

std::string Num(double v, int decimals) {
  char buf[48];
  snprintf(buf, sizeof(buf), "%.*f", decimals, v);
  std::string s = buf;
  if (s.find('.') != std::string::npos) {
    while (s.back() == '0') s.pop_back();
    if (s.back() == '.') s.push_back('0');
  }
  return s;
}

// Paths are written as TOML literal strings ('...', no escapes) unless they contain a quote.
std::string TomlString(const std::string& s) {
  if (s.find('\'') == std::string::npos && s.find('\n') == std::string::npos && s.find('\r') == std::string::npos)
    return "'" + s + "'";
  std::string o = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') o += '\\';
    if (c == '\n') { o += "\\n"; continue; }
    if (c == '\r') { o += "\\r"; continue; }
    o += c;
  }
  return o + "\"";
}

// A TOML number: [+-]digits[.digits][(e|E)[+-]digits], no leading zero on a longer integer part.
bool IsTomlNumber(const std::string& v) {
  size_t i = 0;
  auto digits = [&]() {
    const size_t b = i;
    while (i < v.size() && v[i] >= '0' && v[i] <= '9') ++i;
    return i - b;
  };
  if (i < v.size() && (v[i] == '+' || v[i] == '-')) ++i;
  const size_t int_start = i;
  const size_t n = digits();
  if (n == 0 || (n > 1 && v[int_start] == '0')) return false;
  if (i < v.size() && v[i] == '.') { ++i; if (digits() == 0) return false; }
  if (i < v.size() && (v[i] == 'e' || v[i] == 'E')) {
    ++i;
    if (i < v.size() && (v[i] == '+' || v[i] == '-')) ++i;
    if (digits() == 0) return false;
  }
  return i == v.size();
}

// A value from --apply: booleans and numbers stay bare, quoted text is taken as written, any
// other text becomes a string (a bare word would make the whole file invalid). Something that
// looks like a number but is not a valid TOML one gives "" (the caller exits with 2).
std::string TomlFromArg(const std::string& v) {
  if (v == "true" || v == "false") return v;
  if (!v.empty() && (v.front() == '"' || v.front() == '\'')) return v;
  if (!v.empty() && v.find_first_not_of("0123456789.-+eE") == std::string::npos &&
      v.find_first_of("0123456789") != std::string::npos)
    return IsTomlNumber(v) ? v : std::string();
  if (v.find('\\') != std::string::npos || v.find('/') != std::string::npos || v.find(':') != std::string::npos)
    return TomlString(v);
  std::string o = "\"";
  for (char c : v) {
    if (c == '"' || c == '\\') o += '\\';
    o += c;
  }
  return o + "\"";
}

// ------------------------------------------------------------------------------------------
// system facts: game folder, monitor refresh rate, graphics card
// ------------------------------------------------------------------------------------------
bool HasXex(const std::wstring& dir) {
  if (dir.empty()) return false;
  const DWORD a = GetFileAttributesW((dir + L"\\default.xex").c_str());
  return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring FullPath(const std::wstring& p) {
  wchar_t buf[MAX_PATH * 4];
  const DWORD n = GetFullPathNameW(p.c_str(), DWORD(std::size(buf)), buf, nullptr);
  std::wstring r = (n && n < std::size(buf)) ? std::wstring(buf, n) : p;
  while (r.size() > 3 && (r.back() == L'\\' || r.back() == L'/')) r.pop_back();
  return r;
}

bool IsDir(const std::wstring& p) {
  const DWORD a = p.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(p.c_str());
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool EndsWithI(const std::wstring& s, const wchar_t* suffix) {
  const size_t n = wcslen(suffix);
  return s.size() >= n && _wcsicmp(s.c_str() + s.size() - n, suffix) == 0;
}

// An Xbox 360 disc image (XDVDFS volume at one of the known partition offsets) with default.xex.
bool IsGameIso(const std::wstring& p) {
  if (p.empty() || IsDir(p)) return false;
  return cass::xdvdfs::IsGameIso(std::filesystem::path(p));
}

// The game location: a folder with default.xex, or the game's .iso (played without extracting).
bool IsGameLocation(const std::wstring& p) { return IsDir(p) ? HasXex(p) : IsGameIso(p); }
bool PathExists(const std::wstring& p) {
  return !p.empty() && GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}
// A location meant as a disc image: an existing file, or a (missing) path ending in .iso.
bool IsIsoLocation(const std::wstring& p) {
  return !p.empty() && !IsDir(p) && (PathExists(p) || EndsWithI(p, L".iso"));
}

// The only Xbox game *.iso in `dir`, or "" (none, or more than one).
std::wstring SingleIsoIn(const std::wstring& dir) {
  WIN32_FIND_DATAW fd;
  HANDLE f = FindFirstFileW((dir + L"\\*.iso").c_str(), &fd);
  if (f == INVALID_HANDLE_VALUE) return L"";
  std::wstring found;
  int count = 0;
  do {
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
    const std::wstring p = dir + L"\\" + fd.cFileName;
    if (!EndsWithI(p, L".iso") || !IsGameIso(p)) continue;
    found = p;
    ++count;
  } while (count < 2 && FindNextFileW(f, &fd));
  FindClose(f);
  return count == 1 ? found : L"";
}

// "game" next to this program (a release), then ../../../game (the dev layout: out/build/<config>),
// then a lone game .iso next to this program, then one in ../../.. (the dev root), then one in
// ../../../.. (beside the project folder, where the dev layout keeps it). cass.exe falls back to the
// same places.
std::wstring DetectGameDir() {
  for (const wchar_t* rel : {L"\\game", L"\\..\\..\\..\\game"}) {
    const std::wstring p = FullPath(g_dir + rel);
    if (HasXex(p)) return p;
  }
  for (const wchar_t* rel : {L"", L"\\..\\..\\..", L"\\..\\..\\..\\.."}) {
    const std::wstring iso = SingleIsoIn(FullPath(g_dir + rel));
    if (!iso.empty()) return iso;
  }
  return L"";
}

int g_monitor_hz = 60;

// Refresh rate of the primary display (where the game opens), rounded to the nearest whole
// number: dmDisplayFrequency truncates 59.94 / 143.98 Hz to 59 / 143, so the exact rational
// of the active display path is used when Windows reports it.
void DetectMonitor() {
  MONITORINFOEXW mi{};
  mi.cbSize = sizeof(mi);
  const HMONITOR mon = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
  const bool have_name = mon && GetMonitorInfoW(mon, &mi);
  DEVMODEW dm{};
  dm.dmSize = sizeof(dm);
  if (EnumDisplaySettingsW(have_name ? mi.szDevice : nullptr, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
    g_monitor_hz = int(dm.dmDisplayFrequency);

  UINT32 np = 0, nm = 0;
  if (!have_name || GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS || !np) return;
  std::vector<DISPLAYCONFIG_PATH_INFO> paths(np);
  std::vector<DISPLAYCONFIG_MODE_INFO> modes(nm);
  if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths.data(), &nm, modes.data(), nullptr) != ERROR_SUCCESS) return;
  for (UINT32 i = 0; i < np; ++i) {
    DISPLAYCONFIG_SOURCE_DEVICE_NAME src{};
    src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
    src.header.size = sizeof(src);
    src.header.adapterId = paths[i].sourceInfo.adapterId;
    src.header.id = paths[i].sourceInfo.id;
    if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS || wcscmp(src.viewGdiDeviceName, mi.szDevice) != 0) continue;
    const DISPLAYCONFIG_RATIONAL r = paths[i].targetInfo.refreshRate;
    if (r.Denominator && r.Numerator / r.Denominator > 1)
      g_monitor_hz = int(std::lround(double(r.Numerator) / double(r.Denominator)));
    return;
  }
}

std::string g_auto_rt = "rov";  // render_target_path_d3d12 for the detected card
std::wstring g_gpu_name;

// The first hardware adapter is the one the game's D3D12 device uses. NVIDIA and Intel run the
// ROV path best; AMD needs RTV.
void DetectGpu() {
  IDXGIFactory1* f = nullptr;
  if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&f))) || !f) return;
  for (UINT i = 0;; ++i) {
    IDXGIAdapter1* a = nullptr;
    if (f->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND || !a) break;
    DXGI_ADAPTER_DESC1 d{};
    const bool got = SUCCEEDED(a->GetDesc1(&d));
    a->Release();
    if (!got || (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) || d.VendorId == 0x1414) continue;  // WARP / Basic Render
    g_gpu_name = d.Description;
    g_auto_rt = (d.VendorId == 0x1002 || d.VendorId == 0x1022) ? "rtv" : "rov";
    break;
  }
  f->Release();
}

// ------------------------------------------------------------------------------------------
// the settings
// ------------------------------------------------------------------------------------------
enum Id {
  kDisplay, kResolution, kFiltering,
  kFsr, kSmaa, kSharpness,
  kVsync, kVrr, kLimit, kCap,
  kOverlay, kDedup, kRenderPath, kPso,
  kCount
};
enum class Kind { Segmented, Toggle, Slider };

struct Ctl {
  Id id;
  Kind kind;
  const wchar_t* label;
  const wchar_t* hint;
  std::vector<std::wstring> items;
  int sel = -1;
  bool on = false;
  double val = 0, vmin = 0, vmax = 1, step = 0.1;
  bool dirty = false;
  int rows = 1;            // a segmented control with many items wraps onto this many lines
  int custom = -1;         // index of the "Custom" item, which shows a number box
  int custom_val = 0;      // the number in that box
  bool edited = false;     // the box was typed in since the last load/save
  int custom_min = 0, custom_max = 1000;
  const wchar_t* custom_unit = L"";
  HWND edit = nullptr;
  RectF row{}, ctl{}, box{};
};

struct Card {
  const wchar_t* title;
  std::vector<int> ctls;
  int column;
  const wchar_t* note = nullptr;  // short hint drawn at the bottom of the card
  RectF rect{};
};

Ctl g_c[kCount];
std::vector<Card> g_cards;

// game folder
std::wstring g_game_dir;
bool g_game_ok = false, g_game_auto = false, g_game_dirty = false, g_exe_found = false;

struct ResPreset { int scale; bool native; };
const ResPreset kRes[] = {{1, false}, {2, false}, {2, true}, {3, false}, {3, true}};
const int kLimits[] = {0, 60, 72, 120, 141};  // then Custom

void InitControls() {
  g_c[kDisplay] = {kDisplay, Kind::Segmented, L"Display",
                   L"Fullscreen is borderless (flip model), which keeps G-Sync/FreeSync and tearing control working. Windowed opens a normal window."};
  g_c[kDisplay].items = {L"Fullscreen", L"Windowed"};
  g_c[kResolution] = {kResolution, Kind::Segmented, L"Internal resolution",
                      L"The size the game renders at before it is fitted to your screen. The console rendered 1152x648; 2x and 3x multiply that. 1440p and 4K 3840x2160 also switch the game to native 720p rendering (cass_native_720p), which is EXPERIMENTAL: a faint ghost copy of the shield (and possibly other glow effects) appears slightly off-place. For a clean image pick 1296p or ~4K, which look almost as sharp."};
  g_c[kResolution].items = {L"Original 1152x648", L"1296p", L"1440p", L"~4K 3456x1944", L"4K 3840x2160"};
  g_c[kResolution].rows = 2;
  g_c[kFiltering] = {kFiltering, Kind::Segmented, L"Texture filtering",
                     L"Anisotropic filtering keeps floors and walls sharp at a slant. Game default lets the game choose (the setting is commented out in cass.toml)."};
  g_c[kFiltering].items = {L"Game default", L"4x", L"8x", L"16x"};

  g_c[kFsr] = {kFsr, Kind::Toggle, L"FSR 1.0 upscaling and sharpening",
               L"AMD FidelityFX Super Resolution 1.0 fits the image to your screen (EASU) and sharpens it (RCAS). SMAA runs inside the same pass. Off gives the runtime's plain bilinear stretch, without SMAA."};
  g_c[kSmaa] = {kSmaa, Kind::Toggle, L"Anti-aliasing (SMAA 1x)",
                L"Smooths jagged edges in the rendered image before it is scaled. Needs FSR on."};
  g_c[kSharpness] = {kSharpness, Kind::Slider, L"Sharpness",
                     L"FSR RCAS sharpening: 0 is the sharpest, 2 the softest. 0.5 is the default."};
  g_c[kSharpness].vmin = 0; g_c[kSharpness].vmax = 2; g_c[kSharpness].step = 0.1;

  g_c[kVsync] = {kVsync, Kind::Toggle, L"Vsync",
                 L"Wait for the monitor's refresh on every frame: no tearing and even frame spacing. On a G-Sync/FreeSync monitor you can turn it off, turn on variable refresh rate and set a frame limit a few fps under the refresh rate for lower input lag."};
  g_c[kVrr] = {kVrr, Kind::Toggle, L"Variable refresh rate (allow tearing)",
               L"For G-Sync and FreeSync monitors, only with vsync off. Set a frame limit a few fps under the monitor's refresh rate (for example 141 for 144 Hz)."};
  g_c[kLimit] = {kLimit, Kind::Segmented, L"Frame limit",
                 L"Releases frames on an even timer. With G-Sync/FreeSync set it a few fps under the refresh rate (72 for 75 Hz, 141 for 144 Hz). In the game, F8 switches the limit on and off."};
  g_c[kLimit].items = {L"Off", L"60", L"72", L"120", L"141", L"Custom"};
  g_c[kLimit].custom = 5; g_c[kLimit].custom_min = 1; g_c[kLimit].custom_max = 1000; g_c[kLimit].custom_unit = L"fps";
  g_c[kCap] = {kCap, Kind::Segmented, L"Game frame-rate cap",
               L"The console display rate the game is told about (video_mode_refresh_rate): it can run at most this fast. Set it to your monitor's refresh rate. If the world, cutscenes or physics look too fast above 60, go back to 60."};
  {
    static std::wstring mon;
    mon = L"Monitor (" + std::to_wstring(g_monitor_hz) + L" Hz)";
    g_c[kCap].items = {L"60", mon, L"Custom"};
  }
  g_c[kCap].custom = 2; g_c[kCap].custom_min = 30; g_c[kCap].custom_max = 500; g_c[kCap].custom_unit = L"Hz";

  g_c[kOverlay] = {kOverlay, Kind::Toggle, L"FPS overlay",
                   L"Frame rate and frame time in the top-left corner. F9 toggles it in the game."};
  g_c[kDedup] = {kDedup, Kind::Toggle, L"Skip repeated frames",
                 L"Does not present a frame again when the game has not drawn a new one (menus still repaint). Raises the frame rate and halves the worst frame times. Turn off only to compare."};
  g_c[kRenderPath] = {kRenderPath, Kind::Segmented, L"Render path",
                      L"How render targets are emulated. ROV is fastest on NVIDIA and Intel, RTV is the one for AMD. Auto picks from your graphics card."};
  {
    static std::wstring a;
    a = L"Auto (" + std::wstring(g_auto_rt == "rtv" ? L"RTV" : L"ROV") + L")";
    g_c[kRenderPath].items = {a, L"ROV", L"RTV"};
  }
  g_c[kPso] = {kPso, Kind::Toggle, L"Shader pipeline cache",
               L"Keeps compiled pipelines on disk so later runs do not stutter compiling them again. Rebuilt automatically after a driver or graphics card change."};

  g_cards = {
      {L"DISPLAY", {kDisplay, kResolution, kFiltering}, 0},
      {L"IMAGE", {kFsr, kSmaa, kSharpness}, 0},
      {L"FRAME RATE", {kVsync, kVrr, kLimit, kCap}, 1},
      {L"OVERLAY AND ADVANCED", {kOverlay, kDedup, kRenderPath, kPso}, 1},
  };
}

bool BoolValue(const char* key, bool def) {
  std::string v;
  if (!GetValue(key, &v)) return def;
  return v == "true";
}

int IntValue(const char* key, int def) {
  std::string v;
  if (!GetValue(key, &v) || v.empty()) return def;
  return atoi(v.c_str());
}

void SetEditNumber(Ctl& c);

// config -> controls
void LoadControls() {
  std::string v;
  g_c[kDisplay].sel = BoolValue("fullscreen", false) ? 0 : 1;

  {
    const int scale = IntValue("resolution_scale", 1);
    const bool native = BoolValue("cass_native_720p", false);
    g_c[kResolution].sel = -1;
    for (int i = 0; i < 5; ++i)
      if (kRes[i].scale == scale && kRes[i].native == native) g_c[kResolution].sel = i;
  }

  {
    auto& f = g_c[kFiltering];
    f.sel = 0;
    if (GetValue("anisotropic_override", &v)) f.sel = v == "3" ? 1 : v == "4" ? 2 : v == "5" ? 3 : -1;
  }

  g_c[kFsr].on = BoolValue("cass_fsr", true);
  g_c[kSmaa].on = BoolValue("cass_smaa", true);
  g_c[kSharpness].val = std::clamp(GetValue("cass_fsr_sharpness", &v) ? atof(v.c_str()) : 0.5, 0.0, 2.0);
  g_c[kSharpness].val = std::round(g_c[kSharpness].val * 10.0) / 10.0;

  g_c[kVsync].on = BoolValue("cass_vsync", true);
  g_c[kVrr].on = BoolValue("d3d12_allow_variable_refresh_rate_and_tearing", false);
  {
    auto& l = g_c[kLimit];
    const int fps = (std::max)(0, IntValue("cass_fps_limit", 0));
    l.sel = l.custom;
    l.custom_val = fps > 0 ? fps : 100;
    for (int i = 0; i < 5; ++i)
      if (kLimits[i] == fps) l.sel = i;
  }
  {
    auto& c = g_c[kCap];
    const int hz = IntValue("video_mode_refresh_rate", 60);
    c.custom_val = hz;
    c.sel = hz == 60 ? 0 : hz == g_monitor_hz ? 1 : c.custom;
    if (c.sel != c.custom) c.custom_val = g_monitor_hz != 60 ? g_monitor_hz : 144;
  }

  g_c[kOverlay].on = BoolValue("cass_fps_overlay", false);
  g_c[kDedup].on = BoolValue("cass_present_dedup", true);
  {
    auto& r = g_c[kRenderPath];
    r.sel = 0;
    if (GetValue("render_target_path_d3d12", &v) && v != g_auto_rt) r.sel = v == "rov" ? 1 : v == "rtv" ? 2 : 0;
  }
  g_c[kPso].on = BoolValue("cass_pso_library", true);

  for (auto& c : g_c) c.dirty = c.edited = false;
  for (auto& c : g_c)
    if (c.edit) SetEditNumber(c);

  // Game folder: what cass.toml says, otherwise the first known place that has default.xex
  // (remembered on the next save, so cass.exe finds it without arguments).
  g_game_auto = false;
  g_game_dirty = false;
  g_game_dir.clear();
  if (GetValue("cass_game_dir", &v) && !v.empty()) g_game_dir = Widen(v);
  if (!IsGameLocation(g_game_dir)) {
    // Not set, or the saved folder was moved: try the same places cass.exe falls back to.
    const std::wstring found = DetectGameDir();
    if (!found.empty()) {
      g_game_dir = found;
      g_game_auto = g_game_dirty = true;
    }
  }
  g_game_ok = IsGameLocation(g_game_dir);
}

// controls -> config (only what was changed, plus the rules that keep the file consistent)
void ApplyControls() {
  auto b = [](bool on) { return std::string(on ? "true" : "false"); };
  for (auto& c : g_c) {
    if (!c.dirty) continue;
    switch (c.id) {
      case kDisplay:
        if (c.sel >= 0) SetValue("fullscreen", b(c.sel == 0));
        break;
      case kResolution:
        if (c.sel >= 0) {
          SetValue("resolution_scale", std::to_string(kRes[c.sel].scale));
          SetValue("cass_native_720p", b(kRes[c.sel].native));
        }
        break;
      case kFiltering:
        if (c.sel == 0) CommentOut("anisotropic_override");
        else if (c.sel > 0) SetValue("anisotropic_override", std::to_string(c.sel + 2));
        break;
      case kFsr: SetValue("cass_fsr", b(c.on)); break;
      case kSmaa: SetValue("cass_smaa", b(c.on)); break;
      case kSharpness: SetValue("cass_fsr_sharpness", Num(c.val, 1)); break;
      case kVsync: SetValue("cass_vsync", b(c.on)); break;
      case kVrr: SetValue("d3d12_allow_variable_refresh_rate_and_tearing", b(c.on && !g_c[kVsync].on)); break;
      case kLimit:
        if (c.sel >= 0) SetValue("cass_fps_limit", std::to_string(c.sel == c.custom ? c.custom_val : kLimits[c.sel]));
        break;
      case kCap:
        if (c.sel >= 0)
          SetValue("video_mode_refresh_rate", std::to_string(c.sel == 0 ? 60 : c.sel == 1 ? g_monitor_hz : c.custom_val));
        break;
      case kOverlay: SetValue("cass_fps_overlay", b(c.on)); break;
      case kDedup: SetValue("cass_present_dedup", b(c.on)); break;
      case kRenderPath: {
        static const char* n[] = {nullptr, "rov", "rtv"};
        if (c.sel >= 0) SetValue("render_target_path_d3d12", std::string("\"") + (c.sel == 0 ? g_auto_rt.c_str() : n[c.sel]) + "\"");
      } break;
      case kPso: SetValue("cass_pso_library", b(c.on)); break;
      default: break;
    }
  }
  for (auto& c : g_c) c.dirty = false;
  if (g_game_dirty && !g_game_dir.empty()) {
    SetValue("cass_game_dir", TomlString(Narrow(g_game_dir)));
    g_game_dirty = false;
  }
}

// The runtime's own FXAA would run before SMAA/FSR, so it has to stay off while cass_fsr is on.
void ApplyRules() {
  std::string v;
  if (BoolValue("cass_fsr", true) && !(GetValue("swap_post_effect", &v) && v == "none"))
    SetValue("swap_post_effect", "\"none\"");
  // Tearing / VRR only makes sense without our own vsync.
  if (BoolValue("cass_vsync", true) && BoolValue("d3d12_allow_variable_refresh_rate_and_tearing", false))
    SetValue("d3d12_allow_variable_refresh_rate_and_tearing", "false");
}

// ------------------------------------------------------------------------------------------
// look: dark navy with Captain America red, white and blue
// ------------------------------------------------------------------------------------------
const Color kBg(255, 11, 15, 25), kCard(255, 19, 25, 40), kCardLine(255, 34, 44, 66),
    kText(255, 234, 237, 243), kMuted(255, 140, 151, 173), kTrack(255, 28, 36, 56),
    kTrackHover(255, 40, 51, 77), kAccent(255, 196, 30, 58), kAccentHover(255, 222, 52, 78),
    kBlue(255, 38, 78, 160), kBlueHover(255, 52, 96, 186), kOff(255, 58, 67, 90), kWhite(255, 246, 247, 250),
    kOk(255, 96, 204, 132), kWarn(255, 244, 176, 64);
constexpr COLORREF kEditBg = RGB(28, 36, 56), kEditText = RGB(234, 237, 243);

constexpr float kMinW = 980.f, kMaxW = 6000.f, kMargin = 24.f, kGap = 18.f, kHeaderH = 96.f, kFooterH = 96.f;
constexpr float kPad = 22.f, kTitleH = 44.f, kGameH = 118.f;
constexpr int kBtnCount = 5;  // open settings file, reset, save, play, browse (in the game card)

HWND g_hwnd = nullptr;
HFONT g_edit_font = nullptr;
HBRUSH g_edit_brush = nullptr;
bool g_setting_edit = false;  // ignore EN_CHANGE while the program fills an edit box
float g_scale = 1.f;
float g_win_w = 1060.f;  // logical width; the window can be widened (the height follows the content)
int g_hover_ctl = -1, g_hover_seg = -1, g_hover_btn = -1, g_drag = -1, g_press_btn = -1;
bool g_hover_game = false;
bool g_changed = false;
std::wstring g_status;
Color g_status_color = kMuted;
RectF g_btn[kBtnCount];
RectF g_game_card, g_game_path;
const wchar_t* kBtnText[kBtnCount] = {L"Open settings file", L"Reset to defaults", L"Save", L"Play", L"Browse..."};
float g_content_h = 0;
float g_scroll = 0;  // logical px the cards area is scrolled by (when the screen is too short)

float TotalHeight();
// Logical height of the client area: the whole content when it fits the screen, otherwise the
// work area of the window's monitor (the cards then scroll, the footer stays at the bottom).
float ViewH() {
  RECT cr{};
  if (g_hwnd && GetClientRect(g_hwnd, &cr) && cr.bottom > 0) return float(cr.bottom) / g_scale;
  return TotalHeight();
}
float FooterY() { return ViewH() - kFooterH; }
float MaxScroll() { return (std::max)(0.f, g_content_h - FooterY()); }
// Client-area y of a point in the scrolled part, or far away when it is over the footer.
float ContentY(float y) { return y < FooterY() * g_scale ? y + g_scroll * g_scale : -1e6f; }

bool Hidden(int id) {
  if (id == kSmaa || id == kSharpness) return !g_c[kFsr].on;
  if (id == kVrr) return g_c[kVsync].on;
  return false;
}

bool CustomShown(const Ctl& c) { return c.custom >= 0 && c.sel == c.custom; }

float RowHeight(const Ctl& c) {
  if (c.kind == Kind::Toggle) return 46.f;
  return 70.f + 36.f * float(c.rows - 1) + (CustomShown(c) ? 44.f : 0.f);
}

RectF Scaled(const RectF& r) { return RectF(r.X * g_scale, r.Y * g_scale, r.Width * g_scale, r.Height * g_scale); }

void PlaceEdits() {
  for (auto& c : g_c) {
    if (!c.edit) continue;
    if (!CustomShown(c) || Hidden(c.id)) {
      ShowWindow(c.edit, SW_HIDE);
      continue;
    }
    const RectF b = Scaled(RectF(c.box.X, c.box.Y - g_scroll, c.box.Width, c.box.Height));
    if (b.Y < 0 || b.Y + b.Height > FooterY() * g_scale) {  // scrolled out of view
      ShowWindow(c.edit, SW_HIDE);
      continue;
    }
    const int fh = int(std::lround(18.f * g_scale));
    SetWindowPos(c.edit, nullptr, int(b.X + 10 * g_scale), int(b.Y + (b.Height - float(fh)) / 2), int(b.Width - 20 * g_scale),
                 fh, SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(c.edit, SW_SHOWNA);
  }
}

void Layout() {
  const float colw = (g_win_w - 2 * kMargin - kGap) / 2.f;
  g_game_card = RectF(kMargin, kHeaderH, g_win_w - 2 * kMargin, kGameH);
  g_btn[4] = RectF(g_game_card.X + g_game_card.Width - kPad - 130.f, kHeaderH + kTitleH, 130.f, 36.f);
  g_game_path = RectF(g_game_card.X + kPad, kHeaderH + kTitleH, g_btn[4].X - 12.f - (g_game_card.X + kPad), 36.f);

  const float top = kHeaderH + kGameH + kGap;
  float y[2] = {top, top};
  for (auto& card : g_cards) {
    float h = kTitleH + kPad * 0.4f;
    for (int i : card.ctls) h += Hidden(i) ? 0.f : RowHeight(g_c[i]);
    h += kPad * 0.6f;
    if (card.note) h += 46.f;
    card.rect = RectF(kMargin + card.column * (colw + kGap), y[card.column], colw, h);
    float cy = y[card.column] + kTitleH + kPad * 0.4f;
    for (int i : card.ctls) {
      Ctl& c = g_c[i];
      if (Hidden(i)) { c.row = c.ctl = c.box = RectF(-100, -100, 0, 0); continue; }
      const float rh = RowHeight(c);
      c.row = RectF(card.rect.X + kPad, cy, colw - 2 * kPad, rh);
      if (c.kind == Kind::Toggle) {
        c.ctl = RectF(c.row.X + c.row.Width - 46.f, cy + 10.f, 46.f, 26.f);
      } else if (c.kind == Kind::Segmented) {
        c.ctl = RectF(c.row.X, cy + 26.f, c.row.Width, 34.f + 36.f * float(c.rows - 1));
        c.box = CustomShown(c) ? RectF(c.row.X + 120.f, c.ctl.Y + c.ctl.Height + 8.f, 110.f, 32.f) : RectF(-100, -100, 0, 0);
      } else {
        c.ctl = RectF(c.row.X, cy + 26.f, c.row.Width - 64.f, 34.f);
      }
      cy += rh;
    }
    y[card.column] += h + kGap;
  }
  // Both columns end at the same height: the last card of the shorter column takes the difference.
  const float bottom = (std::max)(y[0], y[1]) - kGap;
  for (int col = 0; col < 2; ++col) {
    for (int k = int(g_cards.size()) - 1; k >= 0; --k) {
      if (g_cards[k].column != col) continue;
      g_cards[k].rect.Height += bottom - (y[col] - kGap);
      break;
    }
  }
  g_content_h = bottom + kGap;
  g_scroll = std::clamp(g_scroll, 0.f, MaxScroll());
  const float bar_y = FooterY() + 2.f;
  const float bw[4] = {170.f, 160.f, 110.f, 130.f};
  float x = g_win_w - kMargin;
  for (int i = 3; i >= 0; --i) {
    x -= bw[i];
    g_btn[i] = RectF(x, bar_y + 14.f, bw[i], 44.f);
    x -= 12.f;
  }
  PlaceEdits();
}

float TotalHeight() { return g_content_h + kFooterH; }

GraphicsPath* Round(const RectF& r, float rad) {
  auto* p = new GraphicsPath();
  const float d = rad * 2;
  p->AddArc(r.X, r.Y, d, d, 180, 90);
  p->AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
  p->AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
  p->AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
  p->CloseFigure();
  return p;
}

void FillRound(Graphics& g, const RectF& r, float rad, const Color& c) {
  SolidBrush br(c);
  GraphicsPath* p = Round(r, rad);
  g.FillPath(&br, p);
  delete p;
}

void StrokeRound(Graphics& g, const RectF& r, float rad, const Color& c, float w = 1.f) {
  Pen pen(c, w);
  GraphicsPath* p = Round(r, rad);
  g.DrawPath(&pen, p);
  delete p;
}

void Text(Graphics& g, const std::wstring& s, const Font& f, const RectF& r, const Color& c,
          StringAlignment h = StringAlignmentNear, StringAlignment v = StringAlignmentCenter,
          bool wrap = false, bool ellipsis = true, bool path_ellipsis = false) {
  StringFormat fmt;
  fmt.SetAlignment(h);
  fmt.SetLineAlignment(v);
  if (!wrap) fmt.SetFormatFlags(StringFormatFlagsNoWrap);
  fmt.SetTrimming(path_ellipsis ? StringTrimmingEllipsisPath : ellipsis ? StringTrimmingEllipsisCharacter : StringTrimmingNone);
  SolidBrush br(c);
  g.DrawString(s.c_str(), -1, &f, r, &fmt, &br);
}

// The round shield: red, white, red rings around a blue disc with a white star.
void Shield(Graphics& g, float cx, float cy, float r) {
  auto disc = [&](float rr, const Color& c) {
    SolidBrush br(c);
    g.FillEllipse(&br, cx - rr, cy - rr, 2 * rr, 2 * rr);
  };
  disc(r, Color(255, 178, 34, 52));
  disc(r * 0.80f, Color(255, 242, 242, 242));
  disc(r * 0.62f, Color(255, 178, 34, 52));
  disc(r * 0.44f, Color(255, 28, 58, 128));
  PointF pts[10];
  for (int i = 0; i < 10; ++i) {
    const float rr = (i % 2 == 0 ? 0.40f : 0.16f) * r;
    const float a = -3.14159265f / 2 + float(i) * 3.14159265f / 5;
    pts[i] = PointF(cx + rr * std::cos(a), cy + rr * std::sin(a));
  }
  SolidBrush star(Color(255, 242, 242, 242));
  g.FillPolygon(&star, pts, 10);
}

std::wstring SliderText(const Ctl& c) {
  wchar_t b[32];
  swprintf(b, 32, L"%.1f", c.val);
  return b;
}

std::wstring GameStatus(Color* color) {
  if (g_game_dir.empty()) {
    *color = kWarn;
    return L"Game not found. Click Browse and pick your game's .iso, or default.xex in the extracted game folder.";
  }
  if (!g_game_ok) {
    *color = kWarn;
    if (!PathExists(g_game_dir))
      return L"The saved game location was not found (moved or deleted?). Click Browse and pick your game's .iso, or default.xex in the extracted game folder.";
    return IsIsoLocation(g_game_dir)
               ? L"This file is not an Xbox 360 game disc image with default.xex. Click Browse to pick another."
               : L"default.xex is not in this folder. Click Browse and pick your game's .iso or default.xex.";
  }
  *color = kOk;
  const bool iso = IsIsoLocation(g_game_dir);
  if (g_game_auto)
    return iso ? L"Disc image found (detected automatically; played without extracting, remembered when you save or play)."
               : L"default.xex found (detected automatically; remembered when you save or play).";
  return iso ? L"Disc image found. The game runs straight from it; nothing is extracted." : L"default.xex found.";
}

// Header links (credit line). Rects are in scrolled content pixels, set by Paint.
struct Link { const wchar_t* label; const wchar_t* url; };
constexpr Link kLinks[2] = {
    {L"YouTube", L"https://www.youtube.com/@BilalReloaded"},
    {L"Patreon", L"https://www.patreon.com/c/bilalreloaded"},
};
const Color kLinkColor(255, 120, 168, 255);
RectF g_link_rect[2];
int g_hover_link = -1;

int HitLink(float x, float content_y) {
  for (int i = 0; i < 2; ++i)
    if (g_link_rect[i].Width > 0 && g_link_rect[i].Contains(x, content_y)) return i;
  return -1;
}

void Paint(HDC hdc, int w, int h) {
  Graphics g(hdc);
  g.SetSmoothingMode(SmoothingModeAntiAlias);
  g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
  g.Clear(kBg);
  FontFamily fam(L"Segoe UI");
  const float S = g_scale;
  const float fy = FooterY() * S;
  Font fTitle(&fam, 22 * S, FontStyleBold, UnitPixel), fSub(&fam, 13 * S, FontStyleRegular, UnitPixel),
      fCard(&fam, 12 * S, FontStyleBold, UnitPixel), fLabel(&fam, 14 * S, FontStyleRegular, UnitPixel),
      fSeg(&fam, 13 * S, FontStyleRegular, UnitPixel), fSegBold(&fam, 13 * S, FontStyleBold, UnitPixel),
      fHint(&fam, 12.5f * S, FontStyleRegular, UnitPixel), fBtn(&fam, 14 * S, FontStyleBold, UnitPixel);

  // header and cards scroll above the footer
  g.SetClip(RectF(0.f, 0.f, float(w), fy));
  g.TranslateTransform(0.f, -g_scroll * S);

  // header: shield, title, a thin red-white-blue rule, and the game status chip
  Shield(g, (kMargin + 24) * S, 46 * S, 24 * S);
  Text(g, L"CAPTAIN AMERICA: SUPER SOLDIER", fTitle, Scaled(RectF(kMargin + 60, 18, 560, 32)), kText);
  {
    // credit line: "PC port by BilalReloaded  ·  YouTube  ·  Patreon" (links open the browser)
    StringFormat measure(StringFormat::GenericTypographic());
    measure.SetFormatFlags(StringFormatFlagsNoWrap | StringFormatFlagsMeasureTrailingSpaces);
    Font fLink(&fam, 13 * S, FontStyleUnderline, UnitPixel);
    const RectF line = Scaled(RectF(kMargin + 60, 48, 560, 22));
    float x = line.X;
    auto piece = [&](const wchar_t* s, const Font& f, Color c) {
      RectF box;
      g.MeasureString(s, -1, &f, PointF(0, 0), &measure, &box);
      const RectF r(x, line.Y, box.Width + 2 * S, line.Height);
      Text(g, s, f, r, c, StringAlignmentNear, StringAlignmentCenter, false, false);
      x += box.Width;
      return r;
    };
    piece(L"PC port by ", fSub, kMuted);
    piece(L"BilalReloaded", fSegBold, kText);
    piece(L"   ·   ", fSub, kMuted);
    for (int i = 0; i < 2; ++i) {
      if (i) piece(L"   ·   ", fSub, kMuted);
      g_link_rect[i] = piece(kLinks[i].label, fLink, g_hover_link == i ? kWhite : kLinkColor);
    }
  }
  {
    const float rx = kMargin + 60, ry = 76, rw = 150;
    SolidBrush red(kAccent), white(Color(255, 220, 224, 232)), blue(kBlue);
    g.FillRectangle(&red, Scaled(RectF(rx, ry, rw / 3, 3)));
    g.FillRectangle(&white, Scaled(RectF(rx + rw / 3, ry, rw / 3, 3)));
    g.FillRectangle(&blue, Scaled(RectF(rx + 2 * rw / 3, ry, rw / 3, 3)));
  }
  {
    const bool ok = g_game_ok && g_exe_found;
    const wchar_t* chip = !g_exe_found ? L"cass.exe not found next to this program"
                          : g_game_ok  ? L"Ready to play"
                                       : L"Game files not found";
    StringFormat measure(StringFormat::GenericTypographic());
    measure.SetFormatFlags(StringFormatFlagsNoWrap | StringFormatFlagsMeasureTrailingSpaces);
    RectF box;
    g.MeasureString(chip, -1, &fSeg, PointF(0, 0), &measure, &box);
    const float cw = box.Width / S + 52.f;
    RectF cr = Scaled(RectF(g_win_w - kMargin - cw, 32, cw, 30));
    FillRound(g, cr, 15 * S, ok ? Color(255, 24, 52, 40) : Color(255, 62, 44, 26));
    SolidBrush dot(ok ? kOk : kWarn);
    g.FillEllipse(&dot, cr.X + 13 * S, cr.Y + 11 * S, 8 * S, 8 * S);
    Text(g, chip, fSeg, RectF(cr.X + 30 * S, cr.Y, cr.Width - 30 * S, cr.Height), ok ? kOk : kWarn,
         StringAlignmentNear, StringAlignmentCenter, false, false);
  }

  auto draw_button = [&](int i) {
    RectF br = Scaled(g_btn[i]);
    const bool over = g_hover_btn == i, down = g_press_btn == i && over;
    const bool disabled = i == 3 && (!g_game_ok || !g_exe_found);
    Color fill = i == 3   ? (disabled ? kOff : (down ? Color(255, 170, 24, 48) : over ? kAccentHover : kAccent))
                 : i == 2 || i == 4 ? (down ? Color(255, 32, 66, 138) : over ? kBlueHover : kBlue)
                                    : (over ? kTrackHover : kTrack);
    FillRound(g, br, 10 * S, fill);
    Text(g, kBtnText[i], fBtn, br, disabled ? kMuted : kWhite, StringAlignmentCenter);
  };

  // game folder card
  {
    RectF cr = Scaled(g_game_card);
    FillRound(g, cr, 12 * S, kCard);
    StrokeRound(g, cr, 12 * S, kCardLine);
    Text(g, L"GAME (FOLDER OR ISO)", fCard, RectF(cr.X + kPad * S, cr.Y + 6 * S, cr.Width - 2 * kPad * S, (kTitleH - 6) * S), kMuted);
    RectF pr = Scaled(g_game_path);
    FillRound(g, pr, 9 * S, kTrack);
    if (!g_game_ok) StrokeRound(g, pr, 9 * S, Color(160, 244, 176, 64));
    Text(g, g_game_dir.empty() ? L"(not set)" : g_game_dir, fLabel, RectF(pr.X + 12 * S, pr.Y, pr.Width - 24 * S, pr.Height),
         g_game_dir.empty() ? kMuted : kText, StringAlignmentNear, StringAlignmentCenter, false, true, true);
    Color sc;
    const std::wstring st = GameStatus(&sc);
    Text(g, st, fHint, RectF(cr.X + kPad * S, pr.Y + pr.Height + 6 * S, cr.Width - 2 * kPad * S, 24 * S), sc);
    draw_button(4);
  }

  // cards
  for (auto& card : g_cards) {
    RectF cr = Scaled(card.rect);
    FillRound(g, cr, 12 * S, kCard);
    StrokeRound(g, cr, 12 * S, kCardLine);
    Text(g, card.title, fCard, RectF(cr.X + kPad * S, cr.Y + 6 * S, cr.Width - 2 * kPad * S, (kTitleH - 6) * S), kMuted);
    if (card.note) {
      Text(g, card.note, fHint, RectF(cr.X + kPad * S, cr.Y + cr.Height - 54 * S, cr.Width - 2 * kPad * S, 44 * S),
           kMuted, StringAlignmentNear, StringAlignmentFar, true);
    }
    for (int i : card.ctls) {
      if (Hidden(i)) continue;
      Ctl& c = g_c[i];
      const RectF row = Scaled(c.row), ct = Scaled(c.ctl);
      const bool hot = g_hover_ctl == i;
      if (c.kind == Kind::Toggle) {
        Text(g, c.label, fLabel, RectF(row.X, row.Y + 2 * S, row.Width - 60 * S, 40 * S), kText);
        FillRound(g, ct, ct.Height / 2, c.on ? (hot ? kAccentHover : kAccent) : (hot ? Color(255, 80, 90, 116) : kOff));
        SolidBrush knob(kWhite);
        const float d = ct.Height - 6 * S;
        g.FillEllipse(&knob, c.on ? ct.X + ct.Width - d - 3 * S : ct.X + 3 * S, ct.Y + 3 * S, d, d);
        continue;
      }
      Text(g, c.label, fLabel, RectF(row.X, row.Y + 2 * S, row.Width, 22 * S), kText);
      // right-hand note on the label line
      std::wstring note;
      Color note_c = kMuted;
      if (c.id == kResolution && c.sel >= 0 && kRes[c.sel].native) { note = L"experimental: ghost shield effect"; note_c = kWarn; }
      else if (c.id == kResolution && c.sel < 0) note = L"custom value in cass.toml";
      else if (c.id == kFiltering && c.sel < 0) note = L"custom value in cass.toml";
      else if (c.id == kRenderPath && !g_gpu_name.empty()) note = g_gpu_name;
      if (!note.empty()) Text(g, note, fHint, RectF(row.X + row.Width * 0.35f, row.Y + 2 * S, row.Width * 0.65f, 22 * S), note_c, StringAlignmentFar);

      if (c.kind == Kind::Segmented) {
        FillRound(g, ct, 9 * S, kTrack);
        const int per = (int(c.items.size()) + c.rows - 1) / c.rows;
        const float sw = ct.Width / float(per), sh = ct.Height / float(c.rows);
        for (size_t k = 0; k < c.items.size(); ++k) {
          RectF seg(ct.X + sw * float(int(k) % per) + 3 * S, ct.Y + sh * float(int(k) / per) + 3 * S, sw - 6 * S, sh - 6 * S);
          const bool sel = int(k) == c.sel;
          const bool over = hot && g_hover_seg == int(k);
          if (sel) FillRound(g, seg, 7 * S, over ? kAccentHover : kAccent);
          else if (over) FillRound(g, seg, 7 * S, kTrackHover);
          // A label that does not fit its segment gets a smaller font instead of being cut off.
          float px = 13.f;
          StringFormat mf;
          mf.SetFormatFlags(StringFormatFlagsNoWrap);
          for (; px > 9.f; px -= 0.5f) {
            Font ft(&fam, px * S, sel ? FontStyleBold : FontStyleRegular, UnitPixel);
            RectF bx;
            g.MeasureString(c.items[k].c_str(), -1, &ft, PointF(0, 0), &mf, &bx);
            if (bx.Width <= seg.Width - 8 * S) break;
          }
          Font fit(&fam, px * S, sel ? FontStyleBold : FontStyleRegular, UnitPixel);
          Text(g, c.items[k], fit, seg, sel ? kWhite : kMuted, StringAlignmentCenter);
        }
        if (CustomShown(c)) {
          const RectF bx = Scaled(c.box);
          Text(g, L"Custom value", fSeg, RectF(row.X, bx.Y, 116 * S, bx.Height), kMuted);
          FillRound(g, bx, 8 * S, kTrack);
          StrokeRound(g, bx, 8 * S, kCardLine);
          wchar_t range[64];
          swprintf(range, 64, L"%s  (%d - %d)", c.custom_unit, c.custom_min, c.custom_max);
          Text(g, range, fHint, RectF(bx.X + bx.Width + 10 * S, bx.Y, row.Width - (bx.X - row.X) - bx.Width - 10 * S, bx.Height), kMuted);
        }
      } else {
        const float t = float((c.val - c.vmin) / (c.vmax - c.vmin));
        const float ty = ct.Y + ct.Height / 2;
        RectF track(ct.X, ty - 3 * S, ct.Width, 6 * S);
        FillRound(g, track, 3 * S, kTrack);
        FillRound(g, RectF(track.X, track.Y, (std::max)(track.Width * t, 6 * S), track.Height), 3 * S, hot || g_drag == i ? kBlueHover : kBlue);
        SolidBrush knob(kWhite);
        const float d = 18 * S;
        g.FillEllipse(&knob, ct.X + ct.Width * t - d / 2, ty - d / 2, d, d);
        Text(g, SliderText(c), fSegBold, RectF(ct.X + ct.Width + 8 * S, ct.Y, 56 * S, ct.Height), kText, StringAlignmentFar);
      }
    }
  }

  g.ResetTransform();
  g.ResetClip();
  if (MaxScroll() > 0) {  // scroll position: a thin thumb on the right edge of the cards area
    const float track = fy - 16 * S, thumb = (std::max)(40 * S, track * fy / (g_content_h * S));
    const float ty = 8 * S + (track - thumb) * (g_scroll / MaxScroll());
    FillRound(g, RectF(float(w) - 9 * S, ty, 5 * S, thumb), 2.5f * S, Color(150, 140, 151, 173));
  }

  // footer: hint / status, buttons
  SolidBrush footer_brush(Color(255, 8, 11, 19));
  g.FillRectangle(&footer_brush, 0.f, fy, float(w), float(h) - fy);
  std::wstring msg = g_status;
  Color mc = g_status_color;
  if (g_hover_ctl >= 0) { msg = g_c[g_hover_ctl].hint; mc = kMuted; }
  else if (g_hover_game) { msg = L"Your game's .iso (played without extracting) or the folder with default.xex. Click to pick a file, right-click to pick a folder. Saved in cass.toml as cass_game_dir."; mc = kMuted; }
  else if (g_hover_btn == 0) { msg = L"Open cass.toml in your text editor. Every setting is explained there; the game's F4 menu edits the same file."; mc = kMuted; }
  else if (g_hover_btn == 1) { msg = L"Put every setting back to the defaults this build shipped with. The game folder is kept and the current file is saved as cass.toml.bak."; mc = kMuted; }
  else if (g_hover_btn == 2) { msg = L"Write the changes to cass.toml. Only the settings shown here are changed; every other line stays as it is."; mc = kMuted; }
  else if (g_hover_btn == 3) {
    msg = !g_exe_found ? L"cass.exe is not next to this program." : !g_game_ok ? L"Choose the game first (.iso or folder)." : L"Save and start the game.";
    mc = (!g_exe_found || !g_game_ok) ? kWarn : kMuted;
  } else if (g_hover_btn == 4) { msg = L"Pick your game's .iso, or default.xex in the extracted folder (right-click the path to pick a folder)."; mc = kMuted; }
  {
    const RectF fr(kMargin * S, fy + 8 * S, (g_btn[0].X - kMargin - 12) * S, (kFooterH - 16) * S);
    StringFormat mfmt;
    mfmt.SetTrimming(StringTrimmingNone);
    float px = 12.5f;
    for (; px > 9.f; px -= 0.5f) {
      Font ft(&fam, px * S, FontStyleRegular, UnitPixel);
      RectF bounds;
      g.MeasureString(msg.c_str(), -1, &ft, RectF(0, 0, fr.Width, 0), &mfmt, &bounds);
      if (bounds.Height <= fr.Height) break;
    }
    Font fit(&fam, px * S, FontStyleRegular, UnitPixel);
    Text(g, msg, fit, fr, mc, StringAlignmentNear, StringAlignmentCenter, true, false);
  }
  for (int i = 0; i < 4; ++i) draw_button(i);
}

// ------------------------------------------------------------------------------------------
// behaviour
// ------------------------------------------------------------------------------------------
void SetStatus(const wchar_t* s, const Color& c) { g_status = s; g_status_color = c; }

void MarkChanged() {
  g_changed = true;
  SetStatus(L"You have unsaved changes.", kWarn);
}

void SetEditNumber(Ctl& c) {
  if (!c.edit) return;
  g_setting_edit = true;
  SetWindowTextW(c.edit, std::to_wstring(c.custom_val).c_str());
  g_setting_edit = false;
}

bool Inside(const RectF& r, float x, float y) { return x >= r.X && x <= r.X + r.Width && y >= r.Y && y <= r.Y + r.Height; }

int HitControl(float x, float y, int* seg) {
  *seg = -1;
  for (int i = 0; i < kCount; ++i) {
    if (Hidden(i)) continue;
    Ctl& c = g_c[i];
    if (!Inside(Scaled(c.row), x, y)) continue;
    if (c.kind == Kind::Segmented) {
      RectF ct = Scaled(c.ctl);
      if (Inside(ct, x, y)) {
        const int per = (int(c.items.size()) + c.rows - 1) / c.rows;
        const int col = (std::min)(int((x - ct.X) / (ct.Width / per)), per - 1);
        const int rw = (std::min)(int((y - ct.Y) / (ct.Height / c.rows)), c.rows - 1);
        const int k = rw * per + col;
        *seg = k < int(c.items.size()) ? k : -1;
      }
    }
    return i;
  }
  return -1;
}

// x, y in client pixels; the footer buttons do not scroll, Browse (4) does.
int HitButton(float x, float y) {
  for (int i = 0; i < kBtnCount; ++i)
    if (Inside(Scaled(g_btn[i]), x, i == 4 ? ContentY(y) : y)) return i;
  return -1;
}

void SetSliderFromX(Ctl& c, float x) {
  RectF ct = Scaled(c.ctl);
  double t = std::clamp(double((x - ct.X) / ct.Width), 0.0, 1.0);
  double v = c.vmin + t * (c.vmax - c.vmin);
  v = std::round(v / c.step) * c.step;
  v = std::clamp(v, c.vmin, c.vmax);
  if (v != c.val) {
    c.val = v;
    c.dirty = true;
    MarkChanged();
  }
}

// Number boxes, only the ones typed in: an empty box (or 0) keeps the previous value, 0 in the frame
// limit box means Off (as in cass.toml), anything else outside the range is pulled into it.
// A value that came from cass.toml and was not touched is written back as it was.
void ReadEdits() {
  for (auto& c : g_c) {
    if (!c.edit || !c.edited) continue;
    c.edited = false;
    wchar_t buf[32] = {};
    GetWindowTextW(c.edit, buf, 32);
    if (buf[0]) {
      const int v = _wtoi(buf);
      if (v == 0) {
        if (c.id == kLimit && CustomShown(c)) { c.sel = 0; c.dirty = true; }  // 0 = Off
        // any other box: 0 means nothing, the previous value stays
      } else {
        const int cv = std::clamp(v, c.custom_min, c.custom_max);
        if (cv != c.custom_val) { c.custom_val = cv; if (CustomShown(c)) c.dirty = true; }
      }
    }
    SetEditNumber(c);
  }
}

bool DoSave() {
  ReadEdits();
  // Start from the file as it is now (the game's F4 menu may have changed it since the window
  // opened), then put only this window's changes into it.
  {
    std::string text;
    if (ReadTextFile(g_cfg_path, text)) {
      g_loaded_text = text;
      g_lines = SplitLines(text, &g_eol);
    }
  }
  ApplyControls();
  ApplyRules();
  if (!SaveConfig()) {
    SetStatus(L"Could not write cass.toml (is the folder writable?).", kWarn);
    return false;
  }
  g_changed = false;
  g_game_auto = false;
  SetStatus(L"Saved. The game reads these settings the next time it starts.", kOk);
  return true;
}

void DoPlay() {
  if (!g_exe_found) {
    MessageBoxW(g_hwnd, L"cass.exe was not found next to this program.", L"Captain America: Super Soldier", MB_OK | MB_ICONWARNING);
    return;
  }
  if (!g_game_ok) {
    MessageBoxW(g_hwnd, L"Choose the game first: its .iso, or the folder that contains default.xex.", L"Captain America: Super Soldier",
                MB_OK | MB_ICONWARNING);
    return;
  }
  if (!DoSave()) return;
  const std::wstring exe = g_dir + L"\\cass.exe";
  // DoSave has just written cass_game_dir, which cass.exe reads as UTF-8 by itself. The runtime
  // turns --game_data_root into a path through the ANSI code page, so it is only passed for a
  // plain-ASCII folder (that also keeps a cass.exe built before cass_game_dir existed working).
  // An .iso is never passed: cass.exe picks it up from cass_game_dir.
  std::wstring cmd = L"\"" + exe + L"\"";
  const bool ascii = std::all_of(g_game_dir.begin(), g_game_dir.end(), [](wchar_t ch) { return ch >= 0x20 && ch < 0x7f; });
  if (ascii && g_game_dir.find(L'"') == std::wstring::npos && IsDir(g_game_dir)) {
    std::wstring dir = g_game_dir;
    // Before the closing quote, n backslashes must be doubled (CommandLineToArgv rules), so a
    // drive root stays "F:\" instead of the drive-relative "F:".
    size_t n = 0;
    while (n < dir.size() && dir[dir.size() - 1 - n] == L'\\') ++n;
    dir.append(n, L'\\');
    cmd += L" --game_data_root=\"" + dir + L"\"";
  }
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, g_dir.c_str(), &si, &pi)) {
    SetStatus(L"Could not start cass.exe.", kWarn);
    return;
  }
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
}

void DoReset() {
  if (MessageBoxW(g_hwnd,
                  L"Put every setting back to its default? The game folder is kept, and your current settings file is saved as cass.toml.bak.",
                  L"Reset to defaults", MB_YESNO | MB_ICONQUESTION) != IDYES)
    return;
  std::string old;
  if (ReadTextFile(g_cfg_path, old)) WriteTextFile(g_cfg_path + L".bak", old);
  g_lines = SplitLines(kDefaultConfig, &g_eol);
  if (!old.empty()) {
    // keep the file's own line ends
    std::string eol;
    SplitLines(old, &eol);
    for (auto& l : g_lines)
      if (!l.eol.empty()) l.eol = eol;
    g_eol = eol;
  }
  SetValue("render_target_path_d3d12", "\"" + g_auto_rt + "\"");  // Auto
  if (!g_game_dir.empty()) SetValue("cass_game_dir", TomlString(Narrow(g_game_dir)));
  ApplyRules();
  g_loaded_text.clear();
  if (!SaveConfig()) {
    SetStatus(L"Could not write cass.toml (is the folder writable?).", kWarn);
    return;
  }
  LoadControls();
  g_changed = false;
  SetStatus(L"Defaults restored.", kOk);
}

void DoOpenFile() {
  if (GetFileAttributesW(g_cfg_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
    g_loaded_text.clear();
    SaveConfig();
  }
  const auto r = INT_PTR(ShellExecuteW(g_hwnd, L"open", g_cfg_path.c_str(), nullptr, g_dir.c_str(), SW_SHOWNORMAL));
  if (r <= 32) {  // no program is associated with .toml
    const std::wstring arg = L"\"" + g_cfg_path + L"\"";
    ShellExecuteW(g_hwnd, L"open", L"notepad.exe", arg.c_str(), g_dir.c_str(), SW_SHOWNORMAL);
  }
}

// The file dialog: an .iso or default.xex (folder == false), or a folder (folder == true).
bool PickGame(HWND owner, bool folder, std::wstring* out) {
  IFileOpenDialog* d = nullptr;
  if (FAILED(CoCreateInstance(__uuidof(FileOpenDialog), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d))) || !d) return false;
  DWORD o = 0;
  d->GetOptions(&o);
  if (folder) {
    d->SetOptions(o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    d->SetTitle(L"Choose the folder that contains default.xex");
  } else {
    const COMDLG_FILTERSPEC filters[] = {
        {L"Xbox 360 game (*.iso; default.xex)", L"*.iso;default.xex"},
        {L"All files (*.*)", L"*.*"},
    };
    d->SetFileTypes(UINT(std::size(filters)), filters);
    d->SetFileTypeIndex(1);
    d->SetOptions(o | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_FILEMUSTEXIST);
    d->SetTitle(L"Choose your game's .iso, or default.xex in the extracted game folder");
  }
  if (!g_game_dir.empty()) {
    const std::wstring start = IsDir(g_game_dir) ? g_game_dir : g_game_dir.substr(0, g_game_dir.find_last_of(L"\\/"));
    IShellItem* si = nullptr;
    if (SUCCEEDED(SHCreateItemFromParsingName(start.c_str(), nullptr, IID_PPV_ARGS(&si))) && si) {
      d->SetFolder(si);
      si->Release();
    }
  }
  bool ok = false;
  if (SUCCEEDED(d->Show(owner))) {
    IShellItem* r = nullptr;
    if (SUCCEEDED(d->GetResult(&r)) && r) {
      PWSTR p = nullptr;
      if (SUCCEEDED(r->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) {
        *out = p;
        CoTaskMemFree(p);
        ok = true;
      }
      r->Release();
    }
  }
  d->Release();
  return ok;
}

// Browse (and a click on the path): an .iso or default.xex. Right-click on the path: a folder.
void DoBrowse(bool folder = false) {
  std::wstring p;
  if (!PickGame(g_hwnd, folder, &p)) return;
  p = FullPath(p);
  if (!IsDir(p) && EndsWithI(p, L"\\default.xex")) p = FullPath(p.substr(0, p.size() - 12));  // its folder
  if (IsDir(p) && !HasXex(p) && HasXex(p + L"\\game")) p += L"\\game";  // picked the folder above it
  if (!IsGameLocation(p)) {
    const std::wstring why =
        IsDir(p) ? L"default.xex was not found in\n" + p +
                       L"\n\nPick your game's .iso, or the folder with default.xex, DATA.0 to DATA.2 and art (the files extracted from your disc)."
                 : L"This file is not an Xbox 360 game disc image with default.xex:\n" + p +
                       L"\n\nPick the game's .iso (the disc image itself), or default.xex in the extracted game folder.";
    MessageBoxW(g_hwnd, why.c_str(), L"Game", MB_OK | MB_ICONWARNING);
    return;
  }
  if (p != g_game_dir || g_game_auto) {
    g_game_dir = p;
    g_game_ok = true;
    g_game_auto = false;
    g_game_dirty = true;
    MarkChanged();
  }
}

float g_max_client_h = 0;  // --max-height=<logical px>: try the scrolling layout of a short screen

// Work area of the monitor the window is on.
RECT WorkArea(HWND h) {
  MONITORINFO mi{};
  mi.cbSize = sizeof(mi);
  if (GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi)) return mi.rcWork;
  RECT r{};
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &r, 0);
  return r;
}

// Client height the window should have: the whole content, but never taller than the work area
// (a 1080p screen at 125-150 % scaling); the cards then scroll and the footer stays visible.
LONG WantClientH(HWND h) {
  RECT wr, cr;
  GetWindowRect(h, &wr);
  GetClientRect(h, &cr);
  const LONG nch = (wr.bottom - wr.top) - cr.bottom;
  const RECT wa = WorkArea(h);
  LONG fit = (wa.bottom - wa.top) - nch;
  if (g_max_client_h > 0) fit = (std::min)(fit, LONG(g_max_client_h * g_scale));  // --max-height
  const LONG min_h = LONG((kHeaderH + kGameH + kFooterH) * g_scale);  // always room for the footer
  return (std::max)(min_h, (std::min)(LONG(TotalHeight() * g_scale), fit));
}

void Redraw() {
  if (!g_hwnd) return;
  Layout();  // controls may have appeared or disappeared; the window height follows the content
  RECT wr, cr;
  GetWindowRect(g_hwnd, &wr);
  GetClientRect(g_hwnd, &cr);
  const LONG want = WantClientH(g_hwnd);
  if (cr.bottom != want) {
    const LONG wh = (wr.bottom - wr.top) - cr.bottom + want;
    const RECT wa = WorkArea(g_hwnd);
    LONG top = wr.top;
    if (top + wh > wa.bottom) top = (std::max)(wa.top, wa.bottom - wh);  // keep the footer on screen
    SetWindowPos(g_hwnd, nullptr, wr.left, top, wr.right - wr.left, wh, SWP_NOZORDER | SWP_NOACTIVATE);
    Layout();  // the footer moved with the new height
  }
  InvalidateRect(g_hwnd, nullptr, FALSE);
}

// Scroll so a (logical) rect of the cards area is visible above the footer.
void ScrollIntoView(const RectF& r) {
  const float top = r.Y - g_scroll, bottom = r.Y + r.Height - g_scroll, view = FooterY();
  if (bottom > view - 8.f) g_scroll += bottom - (view - 8.f);
  else if (top < 8.f) g_scroll -= 8.f - top;
  g_scroll = std::clamp(g_scroll, 0.f, MaxScroll());
  Redraw();
}

void MakeEditFont() {
  if (g_edit_font) DeleteObject(g_edit_font);
  g_edit_font = CreateFontW(-int(std::lround(15.f * g_scale)), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
  for (auto& c : g_c)
    if (c.edit) SendMessageW(c.edit, WM_SETFONT, WPARAM(g_edit_font), TRUE);
}

constexpr int kEditIdBase = 100;

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  switch (m) {
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(h, &ps);
      RECT rc;
      GetClientRect(h, &rc);
      const int cw = rc.right, ch = rc.bottom;
      HDC mem = CreateCompatibleDC(dc);
      HBITMAP bmp = CreateCompatibleBitmap(dc, cw, ch);
      HGDIOBJ old = SelectObject(mem, bmp);
      Paint(mem, cw, ch);
      BitBlt(dc, 0, 0, cw, ch, mem, 0, 0, SRCCOPY);
      SelectObject(mem, old);
      DeleteObject(bmp);
      DeleteDC(mem);
      EndPaint(h, &ps);
      return 0;
    }
    case WM_CTLCOLOREDIT: {
      HDC dc = reinterpret_cast<HDC>(w);
      SetTextColor(dc, kEditText);
      SetBkColor(dc, kEditBg);
      return reinterpret_cast<LRESULT>(g_edit_brush);
    }
    case WM_COMMAND:
      if (HIWORD(w) == EN_CHANGE && !g_setting_edit) {
        const int idx = LOWORD(w) - kEditIdBase;
        if (idx >= 0 && idx < kCount) {
          wchar_t buf[32] = {};
          GetWindowTextW(g_c[idx].edit, buf, 32);
          if (buf[0] && _wtoi(buf) != g_c[idx].custom_val) {
            g_c[idx].edited = true;  // read (and range-checked) when saving
            MarkChanged();
            InvalidateRect(h, nullptr, FALSE);
          }
        }
      }
      return 0;
    case WM_MOUSEMOVE: {
      const float x = float(GET_X_LPARAM(l)), y = float(GET_Y_LPARAM(l));
      if (g_drag >= 0) { SetSliderFromX(g_c[g_drag], x); Redraw(); return 0; }
      const float cy = ContentY(y);
      int seg;
      const int c = HitControl(x, cy, &seg);
      const int b = HitButton(x, y);
      const bool game = b < 0 && Inside(Scaled(g_game_card), x, cy);
      const int ln = b < 0 && y < FooterY() * g_scale ? HitLink(x, cy) : -1;
      if (c != g_hover_ctl || seg != g_hover_seg || b != g_hover_btn || game != g_hover_game ||
          ln != g_hover_link) {
        g_hover_ctl = c; g_hover_seg = seg; g_hover_btn = b; g_hover_game = game; g_hover_link = ln;
        Redraw();
      }
      TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, h, 0};
      TrackMouseEvent(&t);
      return 0;
    }
    case WM_MOUSEWHEEL:
      if (MaxScroll() > 0) {
        g_scroll = std::clamp(g_scroll - float(GET_WHEEL_DELTA_WPARAM(w)) / WHEEL_DELTA * 72.f, 0.f, MaxScroll());
        g_hover_ctl = g_hover_seg = g_hover_btn = g_hover_link = -1;
        g_hover_game = false;
        Redraw();
      }
      return 0;
    case WM_SETCURSOR:
      if (LOWORD(l) == HTCLIENT && g_hover_link >= 0) {
        SetCursor(LoadCursorW(nullptr, IDC_HAND));
        return TRUE;
      }
      break;
    case WM_MOUSELEAVE:
      g_hover_link = -1;
      g_hover_ctl = g_hover_seg = g_hover_btn = -1;
      g_hover_game = false;
      Redraw();
      return 0;
    case WM_LBUTTONDOWN: {
      const float x = float(GET_X_LPARAM(l)), y = float(GET_Y_LPARAM(l));
      SetFocus(h);  // leave a number box
      SetCapture(h);
      const int b = HitButton(x, y);
      if (b >= 0) { g_press_btn = b; Redraw(); return 0; }
      const float cy = ContentY(y);
      if (const int ln = HitLink(x, cy); ln >= 0 && y < FooterY() * g_scale) {
        ReleaseCapture();
        ShellExecuteW(h, L"open", kLinks[ln].url, nullptr, nullptr, SW_SHOWNORMAL);
        return 0;
      }
      if (Inside(Scaled(g_game_path), x, cy)) { ReleaseCapture(); DoBrowse(); Redraw(); return 0; }
      int seg;
      const int i = HitControl(x, cy, &seg);
      if (i < 0) return 0;
      Ctl& c = g_c[i];
      if (c.kind == Kind::Toggle) {
        c.on = !c.on;
        c.dirty = true;
        if (i == kVsync) g_c[kVrr].dirty = true;  // re-evaluated together
        MarkChanged();
      } else if (c.kind == Kind::Segmented) {
        if (seg >= 0 && seg != c.sel) {
          c.sel = seg;
          c.dirty = true;
          MarkChanged();
          if (CustomShown(c) && c.edit) {
            SetEditNumber(c);
            Redraw();
            ScrollIntoView(c.box);
            SetFocus(c.edit);
            SendMessageW(c.edit, EM_SETSEL, 0, -1);
            return 0;
          }
        }
      } else {
        RectF ct = Scaled(c.ctl);
        if (cy >= ct.Y - 6 * g_scale && cy <= ct.Y + ct.Height + 6 * g_scale) { g_drag = i; SetSliderFromX(c, x); }
      }
      Redraw();
      return 0;
    }
    case WM_RBUTTONUP: {
      const float x = float(GET_X_LPARAM(l)), y = float(GET_Y_LPARAM(l));
      if (y < FooterY() * g_scale && Inside(Scaled(g_game_path), x, ContentY(y))) { DoBrowse(true); Redraw(); }
      return 0;
    }
    case WM_LBUTTONUP: {
      ReleaseCapture();
      const float x = float(GET_X_LPARAM(l)), y = float(GET_Y_LPARAM(l));
      const int b = HitButton(x, y);
      const int pressed = g_press_btn;
      g_press_btn = -1;
      g_drag = -1;
      Redraw();
      if (pressed >= 0 && pressed == b) {
        if (b == 0) DoOpenFile();
        else if (b == 1) DoReset();
        else if (b == 2) DoSave();
        else if (b == 3) DoPlay();
        else DoBrowse();
        Redraw();
      }
      return 0;
    }
    case WM_KEYDOWN:
      if (w == VK_ESCAPE) PostMessageW(h, WM_CLOSE, 0, 0);
      else if (w == VK_NEXT || w == VK_PRIOR || w == VK_HOME || w == VK_END) {
        const float page = FooterY() * 0.8f;
        g_scroll = w == VK_HOME ? 0.f : w == VK_END ? MaxScroll() : g_scroll + (w == VK_NEXT ? page : -page);
        g_scroll = std::clamp(g_scroll, 0.f, MaxScroll());
        Redraw();
      }
      else if (w == VK_RETURN) { if (GetKeyState(VK_CONTROL) < 0) DoPlay(); else { DoSave(); Redraw(); } }
      return 0;
    case WM_GETMINMAXINFO: {
      // Wider is allowed (labels get more room); the height follows the content (up to the work
      // area) and is fixed.
      RECT wr, cr;
      GetWindowRect(h, &wr);
      GetClientRect(h, &cr);
      const int ncw = (wr.right - wr.left) - cr.right, nch = (wr.bottom - wr.top) - cr.bottom;
      auto* mm = reinterpret_cast<MINMAXINFO*>(l);
      mm->ptMinTrackSize.x = LONG(kMinW * g_scale) + ncw;
      mm->ptMaxTrackSize.x = LONG(kMaxW * g_scale) + ncw;
      mm->ptMinTrackSize.y = mm->ptMaxTrackSize.y = WantClientH(h) + nch;
      return 0;
    }
    case WM_SIZE:
      if (w != SIZE_MINIMIZED && g_scale > 0) {
        g_win_w = (std::max)(kMinW, (std::min)(kMaxW, float(LOWORD(l)) / g_scale));
        Redraw();
      }
      return 0;
    case WM_DPICHANGED: {
      g_scale = float(HIWORD(w)) / 96.f;
      MakeEditFont();
      const RECT* r = reinterpret_cast<RECT*>(l);
      SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
      Redraw();
      return 0;
    }
    case WM_CLOSE:
      if (g_changed) {
        const int r = MessageBoxW(h, L"Save your changes before closing?", L"Captain America: Super Soldier",
                                  MB_YESNOCANCEL | MB_ICONQUESTION);
        if (r == IDCANCEL) return 0;
        if (r == IDYES && !DoSave()) return 0;
      }
      DestroyWindow(h);
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(h, m, w, l);
}

// Edit boxes get Enter / Esc first: make them save / close like the rest of the window.
LRESULT CALLBACK EditProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
  if (m == WM_KEYDOWN && (w == VK_RETURN || w == VK_ESCAPE)) {
    SetFocus(g_hwnd);
    return SendMessageW(g_hwnd, WM_KEYDOWN, w, l);
  }
  if (m == WM_CHAR && (w == VK_RETURN || w == VK_ESCAPE)) return 0;  // no beep
  return DefSubclassProc(h, m, w, l);
}

std::wstring SelfDir() {
  wchar_t buf[MAX_PATH * 4];
  const DWORD n = GetModuleFileNameW(nullptr, buf, DWORD(std::size(buf)));
  std::wstring p(buf, n);
  return p.substr(0, p.find_last_of(L"\\/"));
}

// --apply key=value ...: write settings without a window (scripts and tests).
int RunApply(const std::vector<std::wstring>& pairs) {
  g_game_dirty = false;  // only what was asked for
  for (const auto& wkv : pairs) {
    const std::string kv = Narrow(wkv);
    const size_t eq = kv.find('=');
    if (eq == std::string::npos || eq == 0) return 2;
    const std::string key = kv.substr(0, eq);
    const std::string value = kv.substr(eq + 1);
    if (key == "resolution") {
      static const char* n[] = {"original", "1296p", "1440p", "3456x1944", "4k"};
      int sel = -1;
      for (int i = 0; i < 5; ++i)
        if (value == n[i]) sel = i;
      if (sel < 0) return 2;
      g_c[kResolution].sel = sel;
      g_c[kResolution].dirty = true;
    } else if (key == "texture_filtering") {
      static const char* n[] = {"default", "4x", "8x", "16x"};
      int sel = -1;
      for (int i = 0; i < 4; ++i)
        if (value == n[i]) sel = i;
      if (sel < 0) return 2;
      g_c[kFiltering].sel = sel;
      g_c[kFiltering].dirty = true;
    } else if (key == "render_path") {
      const int sel = value == "auto" ? 0 : value == "rov" ? 1 : value == "rtv" ? 2 : -1;
      if (sel < 0) return 2;
      g_c[kRenderPath].sel = sel;
      g_c[kRenderPath].dirty = true;
    } else if (key == "game_dir") {
      g_game_dir = FullPath(Widen(value));
      if (!IsGameLocation(g_game_dir)) return 3;  // typo or missing file: write nothing
      g_game_dirty = true;
    } else if (value == "-") {
      CommentOut(key);
    } else {
      const std::string tv = TomlFromArg(value);
      if (tv.empty()) return 2;
      SetValue(key, tv);
    }
  }
  ApplyControls();
  ApplyRules();
  return SaveConfig() ? 0 : 1;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
  g_dir = SelfDir();
  g_cfg_path = g_dir + L"\\cass.toml";

  int argc = 0;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  bool apply = false;
  std::vector<std::wstring> pairs;
  for (int i = 1; argv && i < argc; ++i) {
    const std::wstring a = argv[i];
    if (a == L"--config" && i + 1 < argc) g_cfg_path = FullPath(argv[++i]);
    else if (a.rfind(L"--config=", 0) == 0) g_cfg_path = FullPath(a.substr(9));
    else if (a.rfind(L"--max-height=", 0) == 0) g_max_client_h = float(_wtoi(a.c_str() + 13));
    else if (a == L"--apply") apply = true;
    else if (apply) pairs.push_back(a);
  }
  if (argv) LocalFree(argv);

  DetectMonitor();
  DetectGpu();
  InitControls();
  LoadConfig();
  LoadControls();
  if (apply) return RunApply(pairs);

  g_exe_found = GetFileAttributesW((g_dir + L"\\cass.exe").c_str()) != INVALID_FILE_ATTRIBUTES;
  SetStatus(!g_exe_found ? L"Put this program in the same folder as cass.exe to use the Play button."
            : g_game_dir.empty() ? L"Choose the game to play: its .iso, or the folder with default.xex."
                                 : L"Hover a setting for details. Changes apply the next time the game starts.",
            !g_exe_found || g_game_dir.empty() ? kWarn : kMuted);

  using SetCtx = BOOL(WINAPI*)(HANDLE);
  if (auto fn = reinterpret_cast<SetCtx>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"))) {
    fn(reinterpret_cast<HANDLE>(-4));  // per-monitor v2 (the manifest already asks for it)
  }
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

  GdiplusStartupInput gsi;
  ULONG_PTR token = 0;
  GdiplusStartup(&token, &gsi, nullptr);
  g_edit_brush = CreateSolidBrush(kEditBg);

  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = WndProc;
  wc.hInstance = inst;
  wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
  wc.hIcon = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE));
  wc.hIconSm = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                             GetSystemMetrics(SM_CYSMICON), 0));
  wc.lpszClassName = L"cass_launcher";
  RegisterClassExW(&wc);

  HDC sdc = GetDC(nullptr);
  g_scale = float(GetDeviceCaps(sdc, LOGPIXELSX)) / 96.f;
  ReleaseDC(nullptr, sdc);
  Layout();
  RECT rc{0, 0, LONG(g_win_w * g_scale), LONG(TotalHeight() * g_scale)};
  const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_THICKFRAME | WS_CLIPCHILDREN;
  AdjustWindowRect(&rc, style, FALSE);
  const int ww = rc.right - rc.left, wh = rc.bottom - rc.top;
  g_hwnd = CreateWindowExW(0, wc.lpszClassName,
                           L"Captain America: Super Soldier  -  Port by BilalReloaded", style,
                           (GetSystemMetrics(SM_CXSCREEN) - ww) / 2, (std::max)(0, (GetSystemMetrics(SM_CYSCREEN) - wh) / 2),
                           ww, wh, nullptr, nullptr, inst, nullptr);
  if (!g_hwnd) return 1;
  const UINT dpi = GetDpiForWindow(g_hwnd);
  if (dpi) g_scale = float(dpi) / 96.f;

  // number boxes for the "Custom" choices
  for (int i = 0; i < kCount; ++i) {
    Ctl& c = g_c[i];
    if (c.custom < 0) continue;
    c.edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_NUMBER | ES_AUTOHSCROLL | ES_CENTER, 0, 0, 10, 10, g_hwnd,
                             reinterpret_cast<HMENU>(INT_PTR(kEditIdBase + i)), inst, nullptr);
    SendMessageW(c.edit, EM_LIMITTEXT, 4, 0);
    SetWindowSubclass(c.edit, EditProc, 1, 0);
    SetEditNumber(c);
  }
  MakeEditFont();

  Layout();
  {
    RECT r2{0, 0, LONG(g_win_w * g_scale), LONG(TotalHeight() * g_scale)};
    if (dpi) AdjustWindowRectExForDpi(&r2, style, FALSE, 0, dpi);
    else AdjustWindowRect(&r2, style, FALSE);
    // Centred in the work area and never taller than it (the cards scroll when it is short).
    const RECT wa = WorkArea(g_hwnd);
    const int w2 = r2.right - r2.left;
    const int h2 = (std::min)(int(r2.bottom - r2.top), int(wa.bottom - wa.top));
    SetWindowPos(g_hwnd, nullptr, wa.left + (std::max)(0, int(wa.right - wa.left - w2) / 2),
                 wa.top + (std::max)(0, int(wa.bottom - wa.top - h2) / 2), w2, h2, SWP_NOZORDER);
    Redraw();
  }
  BOOL dark = TRUE;
  DwmSetWindowAttribute(g_hwnd, 20, &dark, sizeof(dark));  // dark title bar
  DwmSetWindowAttribute(g_hwnd, 19, &dark, sizeof(dark));
  ShowWindow(g_hwnd, show);
  UpdateWindow(g_hwnd);

  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  GdiplusShutdown(token);
  if (g_edit_font) DeleteObject(g_edit_font);
  if (g_edit_brush) DeleteObject(g_edit_brush);
  CoUninitialize();
  return 0;
}
