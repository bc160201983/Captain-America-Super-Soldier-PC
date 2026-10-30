// Adapted from gowj (Gears of War: Judgment port) src/gowj_fsr.h (BSD-3-Clause, Copyright (c) 2026 the contributors to this project).
//
// cass - final-image pipeline: SMAA 1x -> AMD FSR 1.0 EASU -> FSR RCAS (see cass_fsr.cpp).

#pragma once

#include <cstdint>

struct ID3D12Resource;

namespace rex::ui {
class ImGuiDrawer;
}  // namespace rex::ui

// Tells the pipeline which texture holds the latest guest output (the presenter's
// D3D12GuestOutputRefreshContext::resource_uav_capable(), left in
// PIXEL_SHADER_RESOURCE) and its size. InstallFsr's own RefreshGuestOutputImpl
// hook calls this; a module that hooks RefreshGuestOutputImpl itself (and so
// prevents ours from finding the slot) must call it from its own wrapper.
extern "C" void CassFsrNoteGuestOutput(ID3D12Resource* resource, uint32_t width,
                                       uint32_t height);

namespace rex::glue {

// Installs the DXGI factory/swap-chain hooks and the presenter refresh hook.
// Call once, before the window and the presenter's swap chain are created
// (ReXApp::OnPreSetup).
void InstallFsr();

// Lets the pipeline stand aside while one of the runtime's ImGui windows
// (F4 settings, ` console, F3 debug, F7 achievements) is on screen, since those
// are painted into the back buffer before Present. Call from OnCreateDialogs.
void SetFsrImGuiDrawer(rex::ui::ImGuiDrawer* drawer);

// Stops the pass for good, waits for its GPU work and releases every D3D12 /
// DXGI reference it holds (guest output, queue, device), so none outlives the
// runtime's device. Call from ReXApp::OnShutdown (UI thread, before the presenter
// and the ImGui drawer are destroyed). Also clears the ImGui drawer pointer.
void ShutdownFsr();

}  // namespace rex::glue
