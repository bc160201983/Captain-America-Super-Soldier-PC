// Adapted from gowj (Gears of War: Judgment ReXGlue port) src/gowj_perf_watch.h (BSD-3-Clause, Copyright (c) 2026 the contributors to this project).
//
// cass - host frame pacing: frame limiter (cass_fps_limit) and redundant-present
// suppression (cass_present_dedup). See cass_pacing.cpp.

#pragma once

#include <cstdint>

namespace rex::glue {

// Hooks D3D12Presenter::PaintAndPresentImpl / RefreshGuestOutputImpl through the
// exported presenter vtable. Call once from CassApp::OnPreSetup, AFTER
// rex::glue::InstallFsr() when that module is used (it finds its slot by the
// pristine export value; this one chains onto it). Idempotent.
// Also installs the system tuning and the FPS overlay (cass_system_tuning.h,
// cass_fps_overlay.h) so a single call wires the whole feature.
void InstallPacing();

// Optional: observer called on the guest render thread for every successful
// guest-output refresh, with the presenter's guest output texture
// (ID3D12Resource*, UAV-capable) and the frontbuffer size. Lets a final-pass
// upscaler (FSR/SMAA) learn which texture holds the frame without hooking the
// same vtable slot a second time. Pass nullptr to clear.
using GuestOutputObserver = void (*)(void* d3d12_resource, uint32_t width, uint32_t height);
void SetGuestOutputObserver(GuestOutputObserver observer);

}  // namespace rex::glue
