// Adapted from gowj (Gears of War: Judgment ReXGlue port) src/gowj_fps_overlay.h (BSD-3-Clause, Copyright (c) 2026 the contributors to this project).
//
// cass - FPS / frame-time overlay (see cass_fps_overlay.cpp).

#pragma once

#include <cstddef>
#include <cstdint>

namespace rex::glue {

// Starts the overlay thread (idempotent). Called by InstallPacing().
void InstallFpsOverlay();
// kind 0 = host present, 1 = guest output refresh (the frames the game really
// rendered). interval_us = time since the previous event of that kind.
void FpsOverlayNote(size_t kind, uint64_t interval_us);

}  // namespace rex::glue
