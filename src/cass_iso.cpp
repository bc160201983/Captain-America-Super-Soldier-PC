// Captain America: Super Soldier - ReXGlue port
//
// ISO mode: see cass_iso.h.

#include "cass_iso.h"

#include <windows.h>

#include <cstdio>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include <rex/filesystem/device.h>
#include <rex/filesystem/devices/disc_image_device.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/vfs.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system.h>

#include "cass_xdvdfs.h"

namespace rex::glue {
namespace {

std::filesystem::path g_iso_path;
std::filesystem::path g_stub_dir;
std::vector<std::string> g_pending_log;  // written before logging existed

// PrepareIsoMode failures: the message box says what is wrong, then the process ends so the SDK
// does not follow up with a second, misleading "--game_data_root was not provided" box.
[[noreturn]] void Fail(const std::string& msg) {
  rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error, msg);
  ExitProcess(1);
}

}  // namespace

std::string PathToUtf8(const std::filesystem::path& p) {
  const std::u8string s = p.u8string();
  return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

bool IsGameIso(const std::filesystem::path& path) { return cass::xdvdfs::IsGameIso(path); }

bool IsoMode() { return !g_iso_path.empty(); }

std::filesystem::path PrepareIsoMode(const std::filesystem::path& iso,
                                     const std::filesystem::path& cache_root) {
  const std::string iso_u8 = PathToUtf8(iso);
  cass::xdvdfs::Image img;
  if (!img.Open(iso)) {
    Fail("Captain America: Super Soldier\n\nThis file is not an Xbox 360 disc image (or it cannot "
         "be read):\n" + iso_u8 + "\n\nChoose the game's .iso or the folder with default.xex in "
         "Captain America Launcher.exe.");
    return {};
  }
  cass::xdvdfs::Image::Entry xex;
  if (!img.FindInRoot("default.xex", &xex) || xex.is_directory() || xex.size == 0) {
    Fail("Captain America: Super Soldier\n\ndefault.xex was not found in this disc image:\n" +
         iso_u8);
    return {};
  }

  std::error_code ec;
  char tag[32];
  std::snprintf(tag, sizeof(tag), "-%llx", static_cast<unsigned long long>(img.size));
  std::filesystem::path stem = iso.stem();
  stem += tag;
  const std::filesystem::path stub = cache_root / "iso" / stem;
  const std::filesystem::path stub_xex = stub / "default.xex";
  const bool reuse = std::filesystem::is_regular_file(stub_xex, ec) &&
                     std::filesystem::file_size(stub_xex, ec) == xex.size && !ec;
  if (!reuse) {
    std::filesystem::create_directories(stub, ec);
    if (!img.Extract(xex, stub_xex)) {
      Fail("Captain America: Super Soldier\n\nCould not copy default.xex out of the disc image "
           "into\n" + PathToUtf8(stub) + "\n\nIs the disk full or the folder read-only?");
      return {};
    }
    g_pending_log.push_back("cass: copied default.xex (" + std::to_string(xex.size) +
                            " bytes) from the ISO to " + PathToUtf8(stub));
  }
  char part[64];
  std::snprintf(part, sizeof(part), "0x%llX", static_cast<unsigned long long>(img.partition));
  g_pending_log.push_back("cass: ISO " + iso_u8 + " (XDVDFS partition at " + part + ", stub " +
                          PathToUtf8(stub) + ")");
  g_iso_path = iso;
  g_stub_dir = stub;
  return stub;
}

void MountIsoImage(rex::Runtime* runtime) {
  if (!IsoMode()) return;
  for (const auto& line : g_pending_log) REXLOG_INFO("{}", line);
  g_pending_log.clear();
  const std::string iso_u8 = PathToUtf8(g_iso_path);

  auto die = [&](const std::string& why) {
    REXLOG_ERROR("cass: cannot run from ISO {}: {}", iso_u8, why);
    rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error,
                              "Captain America: Super Soldier\n\nThe game could not be started "
                              "from the disc image:\n" + iso_u8 + "\n\n" + why);
    TerminateProcess(GetCurrentProcess(), 1);
  };

  auto* fs = runtime ? runtime->file_system() : nullptr;
  if (!fs) return die("the virtual file system is not set up.");

  // game:\ and d:\ are symbolic links to the folder device (\Device\Harddisk0\Partition1). The VFS
  // takes the first registered device whose mount path is a prefix of the path, and the
  // \Device\Harddisk0 NullDevice is registered after Partition1, so a disc device re-registered at
  // Partition1 would be shadowed by it. Like Xenia does for discs, mount the image at
  // \Device\Cdrom0 and point game: and d: there; the stub folder device stays where it is.
  std::string folder_mount;
  if (!fs->FindSymbolicLink("game:", folder_mount)) return die("game:\\ is not mounted.");
  const std::string mount = "\\Device\\Cdrom0";  // the runtime mounts nothing there

  auto disc = std::make_unique<rex::filesystem::DiscImageDevice>(mount, g_iso_path);
  if (!disc->Initialize()) return die("the disc image could not be read (damaged or not an Xbox 360 disc).");
  rex::filesystem::Device* disc_ptr = disc.get();
  if (!fs->RegisterDevice(std::move(disc))) return die("could not mount the disc image at " + mount + ".");
  if (rex::filesystem::Entry* e = fs->ResolvePath(mount + "\\default.xex"); !e || e->device() != disc_ptr) {
    // Another device answers for this path (or the image has no default.xex). The process ends,
    // so the links are left on the folder device.
    return die("default.xex is not readable on the disc image mounted at " + mount + ".");
  }

  // Re-point every drive link that led to the stub folder (game: and d:).
  std::vector<std::string> moved;
  for (const char* link : {"game:", "d:"}) {
    std::string target;
    if (!fs->FindSymbolicLink(link, target) || target != folder_mount) continue;
    fs->UnregisterSymbolicLink(link);
    if (!fs->RegisterSymbolicLink(link, mount)) {
      for (const auto& m : moved) {  // put everything back to the folder device
        fs->UnregisterSymbolicLink(m);
        fs->RegisterSymbolicLink(m, folder_mount);
      }
      fs->RegisterSymbolicLink(link, folder_mount);
      fs->UnregisterDevice(mount);
      return die(std::string("could not point ") + link + " at the disc image.");
    }
    moved.emplace_back(link);
  }

  for (const char* probe : {"game:\\default.xex", "d:\\default.xex"}) {
    rex::filesystem::Entry* e = fs->ResolvePath(probe);
    if (!e || e->device() != disc_ptr) {
      REXLOG_WARN("cass: {} does not resolve through the disc image (resolves to {})", probe,
                  e && e->device() ? e->device()->mount_path() : "nothing");
      if (std::string_view(probe).starts_with("game:")) return die(std::string(probe) + " is not on the disc image.");
    }
  }
  std::string links;
  for (const auto& m : moved) links += (links.empty() ? "" : " and ") + m;
  REXLOG_INFO("cass: running from ISO {} (mounted at {}; {} re-pointed from {})", iso_u8, mount,
              links.empty() ? std::string("no links") : links, folder_mount);
}

}  // namespace rex::glue
