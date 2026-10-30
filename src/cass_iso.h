// Captain America: Super Soldier - ReXGlue port
//
// Running straight from the disc image (.iso) instead of an extracted folder.
//
// The SDK's ReXApp::ConstructRuntime needs game_data_root to be a folder holding default.xex, so
// in ISO mode game_data_root points at a small stub folder under the cache root with only
// default.xex (copied out of the ISO once). Just before the XEX is loaded, OnLoadXexImage mounts
// the SDK's DiscImageDevice on the ISO at \Device\Cdrom0 (as Xenia does for discs) and re-points
// the game: and d: links from the stub folder to it, so the game (and the XEX loader) read
// everything from the disc image.

#pragma once

#include <filesystem>
#include <string>

namespace rex {
class Runtime;
}

namespace rex::glue {

// True if `path` is a regular file that is an Xbox disc image with default.xex in its root.
bool IsGameIso(const std::filesystem::path& path);

// Enters ISO mode for `iso`: makes <cache_root>/iso/<name>-<size>/default.xex (reused when it is
// already there) and returns that folder for PathConfig::game_data_root. On failure shows a
// message box and returns an empty path. Runs before logging is set up; messages are logged later.
std::filesystem::path PrepareIsoMode(const std::filesystem::path& iso,
                                     const std::filesystem::path& cache_root);

bool IsoMode();

// Called from OnLoadXexImage: mounts the ISO in place of the stub folder. No-op outside ISO mode.
// On failure tells the player and ends the process (the stub folder alone cannot run the game).
void MountIsoImage(rex::Runtime* runtime);

std::string PathToUtf8(const std::filesystem::path& p);

}  // namespace rex::glue
