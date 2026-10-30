// Captain America: Super Soldier - ReXGlue port
//
// cass_game_dir: the game folder remembered by Captain America Launcher.exe. See cass_paths.h.

#include "cass_paths.h"

#include "cass_iso.h"

#include <rex/cvar.h>

#include <cwctype>
#include <fstream>
#include <string>
#include <system_error>

// Defined so the runtime knows the key when it loads cass.toml (no "unknown cvar" warning), and
// so --cass_game_dir=<folder> works on the command line too.
REXCVAR_DEFINE_STRING(cass_game_dir, "", "cass",
                      "Game location: the folder with default.xex (DATA.0-2, art) or the game's "
                      ".iso disc image, which is then played without extracting it. Used when "
                      "--game_data_root is not given. Written by Captain America Launcher.exe.");

namespace rex::glue {
namespace {

bool HasXex(const std::filesystem::path& dir) {
  std::error_code ec;
  return !dir.empty() && std::filesystem::is_regular_file(dir / "default.xex", ec);
}

// A folder with default.xex, or an Xbox disc image with default.xex in its root.
bool IsGame(const std::filesystem::path& p) {
  if (p.empty()) return false;
  std::error_code ec;
  if (std::filesystem::is_directory(p, ec)) return HasXex(p);
  return IsGameIso(p);
}

std::filesystem::path Canonical(const std::filesystem::path& p) {
  std::error_code ec;
  const auto canon = std::filesystem::weakly_canonical(p, ec);
  return ec ? p : canon;
}

// The only *.iso in `dir` that is an Xbox game disc, or empty (none, or more than one).
std::filesystem::path SingleIsoIn(const std::filesystem::path& dir) {
  std::error_code ec;
  std::filesystem::path found;
  int count = 0;
  for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    const auto& path = it->path();
    std::wstring ext = path.extension().wstring();
    for (auto& c : ext) c = static_cast<wchar_t>(towlower(c));
    std::error_code fec;
    if (ext != L".iso" || !it->is_regular_file(fec)) continue;
    if (!IsGameIso(path)) continue;
    found = path;
    if (++count > 1) return {};
  }
  return count == 1 ? found : std::filesystem::path();
}

std::filesystem::path FromUtf8(const std::string& s) {
  return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

// Value of `key = '...'` / `key = "..."` on an active line of a TOML file, or "".
std::string ReadTomlString(const std::filesystem::path& file, const std::string& key) {
  std::ifstream in(file, std::ios::binary);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    size_t p = line.find_first_not_of(" \t");
    if (p == std::string::npos || line.compare(p, key.size(), key) != 0) continue;
    p += key.size();
    p = line.find_first_not_of(" \t", p);
    if (p == std::string::npos || line[p] != '=') continue;
    p = line.find_first_not_of(" \t", p + 1);
    if (p == std::string::npos) continue;
    const char q = line[p];
    if (q == '\'') {
      const size_t e = line.find('\'', p + 1);
      if (e != std::string::npos) return line.substr(p + 1, e - p - 1);
    } else if (q == '"') {
      std::string v;
      for (size_t i = p + 1; i < line.size(); ++i) {
        const char c = line[i];
        if (c == '"') return v;
        if (c == '\\' && i + 1 < line.size()) {
          const char n = line[++i];
          v += n == 'n' ? '\n' : n == 't' ? '\t' : n;  // \\ and \" -> the character itself
          continue;
        }
        v += c;
      }
    }
  }
  return {};
}

}  // namespace

std::filesystem::path ResolveGameDir(const std::filesystem::path& config_path) {
  std::filesystem::path from_flag = FromUtf8(REXCVAR_GET(cass_game_dir));  // --cass_game_dir=...
  if (IsGame(from_flag)) return from_flag;
  if (!config_path.empty()) {
    std::filesystem::path from_toml = FromUtf8(ReadTomlString(config_path, "cass_game_dir"));
    if (IsGame(from_toml)) return from_toml;
  }
  const std::filesystem::path exe_dir = config_path.parent_path();
  const std::filesystem::path dev_root = exe_dir / ".." / ".." / "..";
  for (const auto& candidate : {exe_dir / "game", dev_root / "game"}) {
    if (HasXex(candidate)) return Canonical(candidate);
  }
  // The dev layout keeps the ISO beside the project folder: <exe>/../../../.. .
  for (const auto& dir : {exe_dir, dev_root, dev_root / ".."}) {
    const auto iso = SingleIsoIn(dir);
    if (!iso.empty()) return Canonical(iso);
  }
  return {};
}

}  // namespace rex::glue
