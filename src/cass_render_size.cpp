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
// Every function returns its argument unchanged unless cass_native_720p is on, and only remaps
// the exact values the patcher placed, so an unexpected argument passes through.
//
// The size is read when the renderer creates its targets at boot: change the cvar in cass.toml
// and restart the game.

#include <rex/cvar.h>
#include <rex/logging.h>

#include <atomic>

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

std::atomic<bool> g_logged{false};

bool Enabled() {
  const bool on = REXCVAR_GET(cass_native_720p);
  if (on && !g_logged.exchange(true)) {
    REXLOG_INFO("cass_native_720p: rendering at {}x{} (game default {}x{}), EDRAM rebase {}",
                kNativeWidth, kNativeHeight, kGameWidth, kGameHeight,
                REXCVAR_GET(cass_native_720p_edram_rebase) ? "on" : "off");
  }
  return on;
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
