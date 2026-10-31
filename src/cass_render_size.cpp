// Adapted from gow3-recomp src/gow3_game_patches.cpp (BSD-3-Clause, Copyright (c) 2026 Flippage and the gow3-recomp contributors).
// Captain America: Super Soldier - ReXGlue port
//
// Optional native 1280x720 render size. The idea (a post-codegen patch that turns guest
// immediates into calls to host functions behind a cvar) follows the extern "C" game-patch
// pattern of the Gears of War 3 port (tools/patch_game_patches.py + src/gow3_game_patches.cpp);
// no code was copied from it.
//
// The game renders the 3D scene at 1152x648 (renderer config 0x82908778 +0x30/+0x34) and the
// console scales that to the 1280x720 display. tools/patch_native_720p.py rewrites these
// generated sites into calls to the functions below:
//   li 1152 / li 648   0x822A999C/0x822A99A8 (boot init), 0x822501AC/0x822501B0 (Main Clr/Depth
//                      Target descriptors), 0x82251380/0x82251384, 0x8225181C/0x82251820 (re-init),
//                      0x8218A4EC/0x8218A4F0, 0x8218A570/0x8218A574 (copy at 0x82A0BE5C)
//   lfs 1152.0/648.0   0x822513CC/0x822513C8 (render-space ortho matrix)
//   EDRAM tile bases   0x8225ACBC "Half-size depth buffer" 1264, 0x8225AED8 "Quarter-size depth
//                      buffer" 972
//   lis 0x12C          0x822B5984/0x822B59A4 Permanent arena size (memory-system init
//                      sub_822B5868; the Standard heap after it keeps the game's size)
// Every function returns its argument unchanged unless cass_native_720p is on, and only remaps
// the exact values the patcher placed, so an unexpected argument passes through.
//
// The size is read when the renderer creates its targets at boot: change the cvar in cass.toml
// and restart the game. The cvar is latched on first use (the memory-system init, before any
// render target exists), so the arena size and the render size always agree.

#include <rex/cvar.h>
#include <rex/logging.h>

#include <cstdint>

REXCVAR_DEFINE_BOOL(cass_native_720p, false, "GameFixes",
                    "Native 720p: render the 3D scene at 1280x720 instead of the game's 1152x648 "
                    "(which the console upscales). About 23% more pixels to shade. Read when the "
                    "renderer starts; restart after changing it.");
REXCVAR_DEFINE_BOOL(cass_native_720p_edram_rebase, true, "GameFixes",
                    "With cass_native_720p: move the half- and quarter-size depth buffers' EDRAM "
                    "bases so the larger half/quarter colour targets do not overlap them. Turn off "
                    "only to A/B a post-processing glitch.");

namespace {

constexpr int kGameWidth = 1152;
constexpr int kGameHeight = 648;
constexpr int kNativeWidth = 1280;
constexpr int kNativeHeight = 720;

// EDRAM is 2048 tiles of 5120 bytes (80x16 pixels at 32 bpp, no MSAA). The derived colour
// targets (sub_8225AB78) all start at tile 936. At 1152x648 the half-size target (576x648)
// takes 8x41 = 328 tiles, so the half-size depth buffer sits at 936 + 328 = 1264. At 1280x720
// it is 640x720 = 8x45 = 360 tiles, so the depth buffer moves to 936 + 360 = 1296 (ends at 1656).
// The quarter-size target grows from 288x162 to 320x180 = 4x12 = 48 tiles; its depth buffer
// moves from 972 to 936 + 48 = 984.
constexpr int kHalfDepthBaseGame = 1264;
constexpr int kHalfDepthBaseNative = 1296;
constexpr int kQuarterDepthBaseGame = 972;
constexpr int kQuarterDepthBaseNative = 984;

// Guest memory carve (sub_822B5868, once at boot): the "Permanent" bump arena (object
// 0x82A0E974; Alloc sub_8230B420 asserts and returns the out-of-range pointer anyway when full,
// nothing is ever freed), then the "Standard" heap (lis 0x1AC4 = 428.25 MiB), both physically
// allocated top-down with 64 KB pages. Only the arena grows; the heap keeps the game's size,
// because at 720p it already has to hold the larger Main/half/quarter render targets. The extra
// bytes come from guest physical memory outside the carve: 512 MB here, and the carve is 447 MB,
// with no console OS reserve and the XEX image outside it.
constexpr uint32_t kGameArenaSize = 0x012C0000;  // lis 0x12C, 18.75 MiB

// What the Permanent arena holds that follows the render size. Everything else in it is fixed:
// 3 x 0x230000 "frame" sub-arenas (sub_822BBD10), the frontbuffer descriptors and memory-system
// objects, technique/render-state containers, a 32^3 volume and the 64^2..1^2 luminance
// textures of the post-FX module, 0x1FFFE+0x3FFFC+0x4650 (sub_82307CA8) and, at the first level
// load, the 0x4E200 + 0x3A9804 pools of sub_82244390 (where the 720p overflow used to hit).
//   frontbuffer/Color, frontbuffer/Color2 (sub_822B9E28 -> sub_82326F88 with the Permanent
//     allocator): render size, 0x18280106 (8:8:8:8 tiled), 1 level
//   post-FX module (sub_8282EFC0 -> sub_822329F8 -> sub_822B8638(Permanent)): W/3 x H/3 and
//     (W/4+2) x (H/4+2), 0x18280186 (8:8:8:8 tiled), W x H = the Main colour target
// A tiled 2D Xenos texture occupies its size rounded up to 32x32 texels, then to 4 KB.
constexpr uint32_t AlignUp(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }
constexpr uint32_t TiledBytes(uint32_t w, uint32_t h, uint32_t bytes_per_texel) {
  return AlignUp(AlignUp(w, 32) * AlignUp(h, 32) * bytes_per_texel, 4096);
}
constexpr uint32_t ArenaRenderSized(uint32_t w, uint32_t h) {
  return 2 * TiledBytes(w, h, 4) + TiledBytes(w / 3, h / 3, 4) + TiledBytes(w / 4 + 2, h / 4 + 2, 4);
}
// 1152x648: 2 x 0x2F4000 + 0x54000 + 0x3C000 = 0x678000; 1280x720: 2 x 0x398000 + 0x70000 +
// 0x42000 = 0x7E2000. Growth 0x16A000 (1.41 MiB).
constexpr uint32_t kArenaGrowth =
    ArenaRenderSized(kNativeWidth, kNativeHeight) - ArenaRenderSized(kGameWidth, kGameHeight);
static_assert(kArenaGrowth == 0x16A000);
// Margin: at 1152x648 the large contents listed above total 0x119404E (17.58 MiB), which leaves
// 0x12BFB2 (1.17 MiB) for the small allocations plus the game's own slack. 1 MiB covers those
// 1.17 MiB growing with the pixel count as well (+0.27 MiB) more than 3x over. Measured in the
// three 720p crash minidumps (identical): base 0xBE9E0000, 0xFB3C18 used, and the 0x3A9804 pool
// ends 0x9D424 (629 KB) past the old end. The modelled 720p contents before that pool come to
// 0xF5484A, so only ~381 KB of what the arena held was unmodelled. With the extra below, that
// allocation leaves 0x1D2BDC (1.82 MiB) free.
// Rounded to the 64 KB pages the block is allocated with.
constexpr uint32_t kArenaMargin = 0x100000;
constexpr uint32_t kArenaExtra = AlignUp(kArenaGrowth + kArenaMargin, 0x10000);
static_assert(kArenaExtra == 0x270000);
constexpr uint32_t kNativeArenaSize = kGameArenaSize + kArenaExtra;  // 0x01530000, 21.19 MiB
static_assert(kNativeArenaSize % 0x10000 == 0);
constexpr uint32_t kMeasuredOverflow720p = 0x9D424;  // at the 0x3A9804 pool, first level load
static_assert(kGameArenaSize + kMeasuredOverflow720p + 0x100000 <= kNativeArenaSize);

double MiB(uint32_t bytes) { return bytes / (1024.0 * 1024.0); }

// cass_native_720p, read once (thread-safe static). The first caller is the memory-system init
// (before the renderer exists), which also logs the arena size in effect.
bool Enabled() {
  static const bool on = [] {
    const bool v = REXCVAR_GET(cass_native_720p);
    if (v) {
      REXLOG_INFO("cass_native_720p: rendering at {}x{} (game default {}x{}), EDRAM rebase {}",
                  kNativeWidth, kNativeHeight, kGameWidth, kGameHeight,
                  REXCVAR_GET(cass_native_720p_edram_rebase) ? "on" : "off");
    }
    return v;
  }();
  return on;
}

// Both arena sites (physical alloc size and arena end) get this one value.
uint32_t ArenaSize() {
  static const uint32_t size = [] {
    const bool native = Enabled();
    const uint32_t s = native ? kNativeArenaSize : kGameArenaSize;
    if (native) {
      REXLOG_INFO("cass memory: Permanent arena {:#010x} ({:.2f} MiB, game {:#010x} + {:#x} for "
                  "cass_native_720p, from free guest physical memory); Standard heap unchanged",
                  s, MiB(s), kGameArenaSize, kArenaExtra);
    } else {
      REXLOG_INFO("cass memory: Permanent arena {:#010x} ({:.2f} MiB) (game default)", s, MiB(s));
    }
    return s;
  }();
  return size;
}

}  // namespace

extern "C" int CassRenderWidth(int orig) {
  return (orig == kGameWidth && Enabled()) ? kNativeWidth : orig;
}

extern "C" int CassRenderHeight(int orig) {
  return (orig == kGameHeight && Enabled()) ? kNativeHeight : orig;
}

extern "C" float CassRenderWidthF(float orig) {
  return (orig == float(kGameWidth) && Enabled()) ? float(kNativeWidth) : orig;
}

extern "C" float CassRenderHeightF(float orig) {
  return (orig == float(kGameHeight) && Enabled()) ? float(kNativeHeight) : orig;
}

extern "C" uint32_t CassRenderArenaSize(uint32_t orig) {
  return orig == kGameArenaSize ? ArenaSize() : orig;
}

extern "C" int CassRenderEdramBase(int orig) {
  if (!Enabled() || !REXCVAR_GET(cass_native_720p_edram_rebase)) return orig;
  switch (orig) {
    case kHalfDepthBaseGame:
      return kHalfDepthBaseNative;
    case kQuarterDepthBaseGame:
      return kQuarterDepthBaseNative;
    default:
      return orig;
  }
}
