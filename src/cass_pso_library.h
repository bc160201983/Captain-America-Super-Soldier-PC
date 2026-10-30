// Adapted from gowj (Gears of War: Judgment PC port) src/gowj_pso_library.h (BSD-3-Clause, Copyright (c) 2026 the contributors to this project).
//
// cass - persistent D3D12 pipeline library (see cass_pso_library.cpp).

#pragma once

#include <filesystem>

namespace rex::glue {

// Hooks ID3D12Device::CreateGraphicsPipelineState so pipelines are answered from an
// ID3D12PipelineLibrary stored as <cache_dir>/pso_library.bin. Call once, before the
// runtime creates its D3D12 device (CassApp::OnPreSetup). An empty cache_dir falls back
// to "<exe dir>/cache". Does nothing when the cass_pso_library cvar is false or the
// CASS_PSOLIB environment variable is "0".
void InstallPsoLibrary(const std::filesystem::path& cache_dir);

// Writes the library to disk now if anything new was stored since the last save
// (the background writer otherwise saves every 30 s), then stops all further use of the
// library (no more saves or loads). Safe when the library was never installed. Call once
// from CassApp::OnShutdown, while the D3D12 device is still alive.
void FlushPsoLibrary();

}  // namespace rex::glue
