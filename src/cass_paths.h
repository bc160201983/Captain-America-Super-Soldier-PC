// Captain America: Super Soldier - ReXGlue port
//
// Game location for cass.exe started without --game_data_root (double-click, old shortcut):
// --cass_game_dir, then cass_game_dir in cass.toml (written by Captain America Launcher.exe), then
// a "game" folder next to cass.exe, then ../../../game (the dev layout, out/build/<config>), then
// the only Xbox game *.iso next to cass.exe, then the only one in ../../.., then in ../../../.. (the
// ISO beside the project folder). The first two may name a folder with default.xex or a disc image (.iso, played without extracting it; see cass_iso.h).

#pragma once

#include <filesystem>

namespace rex::glue {

// Returns the first candidate (a folder that contains default.xex, or an Xbox game disc image),
// or an empty path.
// `config_path` is cass.toml; it is read with a small line parser because the runtime has not
// loaded it yet when paths are configured.
std::filesystem::path ResolveGameDir(const std::filesystem::path& config_path);

}  // namespace rex::glue
