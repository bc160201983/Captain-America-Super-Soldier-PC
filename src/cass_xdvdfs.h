// Captain America: Super Soldier - ReXGlue port
//
// Minimal read-only XDVDFS (Xbox 360 disc image) reader: finds the game partition and looks up a
// file in the root directory. Header-only and free of runtime dependencies so both cass.exe and
// Captain America Launcher.exe use it. The full ISO is mounted by the SDK's DiscImageDevice; this
// only checks an image and pulls default.xex out of it.

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace cass::xdvdfs {

constexpr uint64_t kSector = 2048;
// XGD3, XGD2, XGD1 and a plain (game partition only) image, in the order Xenia probes them.
constexpr uint64_t kPartitionOffsets[] = {0x0, 0x2080000, 0xFD90000, 0x18300000};
constexpr char kMagic[] = "MICROSOFT*XBOX*MEDIA";  // 20 bytes

struct Image {
  std::ifstream file;
  uint64_t size = 0;
  uint64_t partition = 0;
  uint32_t root_sector = 0;
  uint32_t root_size = 0;

  bool Read(uint64_t offset, void* out, size_t n) {
    if (offset > size || n > size - offset) return false;
    file.clear();
    file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    file.read(static_cast<char*>(out), static_cast<std::streamsize>(n));
    return file.good() || static_cast<size_t>(file.gcount()) == n;
  }

  // Opens `path` and finds the XDVDFS volume descriptor. False if the file is not an Xbox disc.
  bool Open(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) return false;
    size = std::filesystem::file_size(path, ec);
    if (ec) return false;
    file.open(path, std::ios::binary);
    if (!file) return false;
    for (const uint64_t off : kPartitionOffsets) {
      uint8_t vd[28];
      if (!Read(off + 32 * kSector, vd, sizeof(vd))) continue;
      if (std::memcmp(vd, kMagic, 20) != 0) continue;
      partition = off;
      std::memcpy(&root_sector, vd + 20, 4);
      std::memcpy(&root_size, vd + 24, 4);
      if (root_size == 0 || root_size > 64 * 1024 * 1024) return false;
      return true;
    }
    return false;
  }

  struct Entry {
    uint32_t sector = 0;
    uint32_t size = 0;
    uint8_t attributes = 0;
    bool is_directory() const { return attributes & 0x10; }
  };

  // Case-insensitive lookup of `name` in the root directory (walks the whole entry tree).
  bool FindInRoot(std::string_view name, Entry* out) {
    std::vector<uint8_t> dir(root_size);
    if (!Read(partition + uint64_t(root_sector) * kSector, dir.data(), dir.size())) return false;
    auto lower = [](char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; };
    std::vector<uint32_t> stack{0};
    std::vector<bool> seen(dir.size() / 4 + 1, false);
    while (!stack.empty()) {
      const uint32_t at = stack.back();
      stack.pop_back();
      if (at + 14 > dir.size() || seen[at / 4]) continue;
      seen[at / 4] = true;
      const uint8_t* e = dir.data() + at;
      uint16_t left, right;
      std::memcpy(&left, e, 2);
      std::memcpy(&right, e + 2, 2);
      if (left == 0xFFFF && right == 0xFFFF) continue;  // padding
      const uint8_t name_len = e[13];
      if (at + 14 + name_len > dir.size()) continue;
      if (name_len == name.size()) {
        bool eq = true;
        for (size_t i = 0; i < name.size() && eq; ++i) eq = lower(char(e[14 + i])) == lower(name[i]);
        if (eq) {
          std::memcpy(&out->sector, e + 4, 4);
          std::memcpy(&out->size, e + 8, 4);
          out->attributes = e[12];
          return true;
        }
      }
      if (left && left != 0xFFFF) stack.push_back(uint32_t(left) * 4);
      if (right && right != 0xFFFF) stack.push_back(uint32_t(right) * 4);
    }
    return false;
  }

  // Copies a file's bytes to `dest` (through a temporary file, so a half-written copy never
  // looks complete).
  bool Extract(const Entry& entry, const std::filesystem::path& dest) {
    const uint64_t start = partition + uint64_t(entry.sector) * kSector;
    if (start > size || entry.size > size - start) return false;
    std::filesystem::path tmp = dest;
    tmp += ".part";
    {
      std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
      if (!o) return false;
      std::vector<char> buf(1 << 20);
      uint64_t done = 0;
      while (done < entry.size) {
        const size_t n = size_t(std::min<uint64_t>(buf.size(), entry.size - done));
        if (!Read(start + done, buf.data(), n)) return false;
        o.write(buf.data(), static_cast<std::streamsize>(n));
        if (!o) return false;
        done += n;
      }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, dest, ec);
    if (ec) {
      std::filesystem::remove(tmp, ec);
      return false;
    }
    return true;
  }
};

// True if `path` is an Xbox disc image whose root holds default.xex.
inline bool IsGameIso(const std::filesystem::path& path) {
  Image img;
  Image::Entry e;
  return img.Open(path) && img.FindInRoot("default.xex", &e) && !e.is_directory() && e.size > 0;
}

}  // namespace cass::xdvdfs
