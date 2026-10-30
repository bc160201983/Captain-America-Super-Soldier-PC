# Captain America: Super Soldier (53450858): plan for frame rate, resolution and performance

Base: stock ReXGlue SDK 0.10.0.9-dev (nightly 923c1a59). This is the same commit that gowj runs stock and that gow2/gow3 patch.

**Three facts from the findings change the plan:**
- **The game is not capped at 30 fps.** It sets PresentInterval ONE, so it is held to the SDK's 60 Hz guest vblank. It also has its own adaptive vsync in `sub_82251FD8`: vsync turns off when a frame takes more than 33.4 ms. No interval-2 value is used anywhere. The comment in `config/cass.toml` that says "The console version runs at 30 fps" is wrong.
- **The game renders below 720p.** The internal resolution is 1152x648, and the output is 1280x720 (struct at 0x82908778). So `resolution_scale=2` gives 2304x1296 internally, not 1440p. The cass.toml comment "multiple of the game's 1280x720" is also wrong.
- **There is no community patch for 53450858** in xenia-canary/game-patches (520 files checked). Every game-side address below comes from our own disassembly of `image.bin`.

---

## 1. Do now (stock SDK flags)

Put GPU-plugin flags in `out/build/release/cass.toml` (the source copy is `config/cass.toml`), on the CLI (`--name=value`), or in `REX_<NAME>` env vars. Do not put them in `OnPreSetup` through `SetFlagByName`. Stock rejects cvars that are not registered yet. `cass_app.h:47-54` `SetGpuFlag` only works because of its `_putenv_s` fallback (`RegisterFlag` applies `REX_*` env at late registration).

**Already in cass.toml, keep as is:**
`resolution_scale=2`, `direct_host_resolve=true`, `resolve_resolution_scale_fill_half_pixel_offset=true`, `mrt_edram_used_range_clamp_to_min=true`, `present_dither=true`, `anisotropic_override=5`, `render_target_path_d3d12="rov"`, `execute_unclipped_draw_vs_on_cpu=true`. `cass_app.h:60-61` forces `gpu_allow_invalid_fetch_constants=true` (Xenia #1321 says this is required, or you get pink screens) and `d3d12_submit_on_primary_buffer_end=true`.

**Frame rate (do the steps in order and test after each):**
```toml
# Step A: gentle unlock. CA uses PresentInterval ONE, so the cap equals the guest vblank rate.
vsync = true
video_mode_refresh_rate = 144          # or the monitor's Hz; the cap becomes 144 instead of 60
# Step B, only if A does not raise fps: 1 kHz guest vblank, so ONE flips about 1 ms later (effectively uncapped)
# vsync = false
d3d12_allow_variable_refresh_rate_and_tearing = true   # stock always presents interval 0; VRR/G-Sync hides the tearing
fullscreen = true
fullscreen_exclusive = false           # borderless flip model keeps VRR/tearing working
```
For a cap without host code, use the NVIDIA driver "Max Frame Rate" set just below the refresh rate. Stock has no host limiter cvar.

**Resolution and AA:**
```toml
resolution_scale = 2                   # 2304x1296 internal. Try 3 (3456x1944); go back to 2 on colour or speckle problems (Gears 3/Judgment broke at 3)
# swap_post_effect = "fxaa"            # cheap AA only at scale 1-2; "none" at 3. It also softens the HUD
use_fuzzy_alpha_epsilon = true         # stops alpha-test flicker on NVIDIA (Judgment uses it)
native_2x_msaa = true                  # default; keep
present_letterbox = true               # default; 16:9 on non-16:9 displays
present_effect = "bilinear"            # the only value in the stock build (no FSR/CAS compiled in)
```
After launch, check the log for `D3D12 draw resolution scaling is enabled, but the swap source is unscaled`. If it appears, the scale is not reaching the final image.

**Caches and memory:**
```toml
texture_cache_memory_limit_soft = 2048
texture_cache_memory_limit_hard = 4096
texture_cache_memory_limit_render_to_texture = 256   # scaled resolves grow with scale squared
texture_cache_memory_limit_soft_lifetime = 120       # fewer evictions and reloads, so less stutter
async_shader_compilation = true                      # default; skips draws instead of blocking on a pipeline
store_shaders = true                                 # default; persistent shader and pipeline-description storage
d3d12_pipeline_creation_threads = 16                 # 9950X3D: drains first-run compiles faster (-1 auto is fine too)
cache_root = "cache"                                 # optional: portable cache next to cass.exe, off OneDrive
```

**CPU/GPU synchronisation (test one at a time against default):**
```toml
clear_memory_page_state = false        # Gears 2 went 122 -> 240 fps. Revert if textures or geometry go stale
readback_memexport = false             # Gears 3 measured long CPU waits. Revert if particles or skinning break
readback_resolve = "none"              # default; never "full"
# occlusion_query_enable = false       # A/B only. Each query end waits on a fence; risk: flares or objects not culled
```

**Keep at default:** `d3d12_tiled_shared_memory=true` (the 2 GB tiled buffer for scaled resolves). gowj sets it false, but the SDK analysis says scaling uses it. `d3d12_bindless=true`, `d3d12_queue_priority=1`, `ignore_thread_priorities=true`, `ignore_thread_affinities=true`, `clock_no_scaling=false` and `clock_source_raw=false` (changing the guest clock is not a valid unlock), plus all debug and dump flags off.

**Measurement:**
```toml
log_level = "warn"                     # less logging I/O on hot paths
# perf_log_csv = "frames.csv"          # gowj reports it wrote no file; use host code item 2.4 if it is empty
```
Do not add `present_fsr_*`, `present_cas_additional_sharpness` or any `vulkan_*` key. They are absent from the shipped DLLs and only produce "Config: unknown cvar".

---

## 2. Host code we can write (public SDK APIs and exported symbols, stock DLLs)

| # | Item | Adapt from | CA-specific input |
|---|---|---|---|
| 2.1 | **Force vsync off in the game.** Override guest `sub_823CE6D8` (the PresentInterval setter `stw r4,0x3630(r3); blr`) so it always stores `0x80000000` (IMMEDIATE), behind a host cvar such as `cass_unlock_fps`. No image bytes change. | `REX_HOOK_RAW`/`REX_EXTERN` pattern in `tools/ref/gow2/src/gow2_engine_tick_hook.cpp`, or `FunctionDispatcher::SetFunction` (`include/rex/system/function_dispatcher.h:47`) as in `gow2_frame_stats.cpp` | 0x823CE6D8, called from 0x822520FC (game) and 0x823EF2C0 (D3D reset) |
| 2.2 | **Frame limiter / pacer.** Waitable-timer (HIGH_RESOLUTION) sleep plus a final spin, on a QPC grid, as a live cvar. Pairs with Step B or 2.1. | `tools/ref/gowj/src/gowj_perf_watch.cpp` `PaceToGrid` (~L590-703), installed through the `D3D12Presenter` vtable (`RefreshGuestOutputImpl` export, ~L960-1040); exports confirmed in our `rexruntime.dll`. Alternative: `gow2_fps_cap.cpp/.h` hooked on a per-frame guest function | None for the gowj presenter variant. For the gow2 variant: `sub_822AC030` (per-frame update) |
| 2.3 | **Redundant-present suppression.** Skip `PaintAndPresentImpl` when there is no new guest frame and fewer than 33.3 ms have passed (Judgment went 51 -> 61 fps and p99 71 -> 31 ms). Also do not keep persistent ImGuiDialogs alive (they cause the stock repaint-at-display-rate bug). | `gowj_perf_watch.cpp` `HookPresent` L347-455 | none |
| 2.4 | **Measurement:** cvar registry dump (do this first, to confirm which TOML keys are real), FPS overlay, PERFWATCH percentiles, `rex::perf` counters and a CSV through the exported `?SetCsvLogPath@perf@rex`. | `gowj_app.h` `DumpCvarRegistry` L653-690; `gowj_fps_overlay.cpp`; `gowj_perf_watch.cpp` | none |
| 2.5 | **Persistent ID3D12PipelineLibrary** (removes PSO compile hitches on repeat visits). | `tools/ref/gowj/src/gowj_pso_library.cpp` (nearly verbatim). Call `InstallPsoLibrary()` from `OnPreSetup` | none |
| 2.6 | **SMAA 1x + FSR1 EASU/RCAS final pass.** The stock-SDK way to get FSR/CAS-class output. Set `swap_post_effect="none"` when it is on. | `gowj_fsr.cpp` + `src/fsr/*.hlsl` (DXGI factory/swap-chain IAT and vtable hooks; MIT/BSD) | none |
| 2.7 | **Process power-throttling opt-out** (EcoQoS off, keeps timer resolution). | `gowj_system_tuning.cpp` (about 10 lines) | none |
| 2.8 | **Fastest-adapter pick** (`d3d12_adapter` = the adapter with the most VRAM). | `gowj_app.h` `PickFastestAdapter` L304-338 | none |
| 2.9 | **Cache and user dirs in %LOCALAPPDATA%** (avoids OneDrive resync stalls on shader-store writes). | `gowj_app.h` `OnConfigurePaths` L560-610; `gow3_app.h` L160-179 | none |
| 2.10 | **Build-side.** Post-codegen memory-barrier fences (also fixes the `isync` → `REX_UNIMPLEMENTED` throw); an x64 version of spin hints (`_mm_pause()` for db*cyc); PGO of the generated code once codegen is stable. | `tools/ref/gow2/tools/patch_memory_barriers.py`, `patch_spin_hints.py` (ARM-only; port the idea); `gowj CMakeLists.txt` L60-86 + `src/gowj_pgo.cpp` | none |
| 2.11 | Optional: auto quality tiers and a self-lowering tuner. | `gowj_quality.cpp/.h` | none |

---

## 3. Needs a patched SDK (self-built from 923c1a59 with `tools/ref/gow2/scripts/build-sdk.ps1`; gow3 numbers its patches differently)

| Patch | What it does | Value for CA | Cost |
|---|---|---|---|
| gow2 `sdk/patches/0025-pipeline-no-stall.patch` | Adds `d3d12_pipeline_wait_at_submission_end=false` and moves depth-only PSOs to worker threads. In Gears, p99.9 went 321 ms -> 20 ms. | Highest. Biggest stutter fix; no host equivalent. | Medium |
| `0027-d3d12-pipeline-library.patch` | In-SDK PSO library. | Same benefit as host item 2.5; pick one. | Low |
| `0031-ui-paint-on-demand.patch` (gow3 0039) | One present per guest frame. | High if any ImGui dialog stays alive. Host item 2.3 is the workaround. | Low |
| `0021-d3d12-host-vsync.patch` | `Present(1)` without tearing when `vsync=true`. | Only for users without VRR. Note it ties host vsync to guest vblank pacing. | Low |
| `0035-runtime-defaults-for-late-cvars` (gow3 0071) | `SetFlagByName` works for GPU cvars in `OnPreSetup`. | Convenience; the TOML is the workaround. | Low |
| `0017` `d3d12_stats_log` | Draws, skipped draws, PSO and memexport statistics. | Tuning aid. | Low |
| `0059` vertex buffer requested every draw | Fixes stale dynamic VBs. | Correctness; check if flicker appears. | Low |
| gow3 `0062`/`0063` occlusion queries | Stock answers "visible" to everything after the first mismatched query. | Only if CA uses GPU queries (check stats). | Medium |
| `-DREXGLUE_ENABLE_FIDELITYFX=ON` (build option, not a patch) | Unlocks `present_effect` fsr/cas and the `present_fsr_*`/`present_cas_*` cvars. | Same benefit as host item 2.6. | Low (rebuild only) |
| `0024` NVIDIA ROV default | Already achieved with `render_target_path_d3d12="rov"`. | Skip. | — |
| `0033` | Already achieved with `execute_unclipped_draw_vs_on_cpu=true`. | Skip. | — |
| `0032-0045` native surfaces and resolves | Up to about 2x fps in gow3. | Later. Tuned to Gears' copies; risk of black screens. | High |
| `0026`/`0039` 12-bit EDRAM bases | Needed for targets wider than 1280 / ultrawide. | Only if we do ultrawide. | Medium |
| `0002`/`0030` temporal FSR3/FSR4/DLSS | Needs CA's view-projection VS registers, depth convention and HUD split; ghosting. Judgment shelved it. | Low priority. | Very high |

Owning a forked SDK DLL set is a recurring maintenance cost. Only 0025 has no host-code alternative.

---

## 4. Game-specific patches for 53450858

All of these come from our own capstone disassembly of `F:/xbox recomp/ca-recomp/out/image/image.bin` (base 0x82000000). **None has been tested at runtime.**

**4.1 Frame rate (high confidence on the mechanism; the original bytes were checked)**
- Cap path: game `sub_82251FD8` (called each frame from `sub_822AC030` at 0x822AC080) → setter `0x823CE6D8` stores PresentInterval at `dev+0x3630`. `sub_823E0A48` @0x823E0AF8-0x823E0B54 encodes it as a vblank interval (0/1 → 1, 2 → 2, 4 → 3, IMMEDIATE → 0). The vblank ISR is `0x823E06D0` and the flip handler is `0x823E07F0`. The game's debug cvar `g_EnableVSync` ("Rendering/VSync", value byte at 0x828E5C2C, default 0) forces vsync on.
- **Patch A (preferred):** `0x822520F0`: `38800001` (li r4,1) → `3C808000` (lis r4,0x8000). This always passes IMMEDIATE. The host-code equivalent is item 2.1 (override `sub_823CE6D8`).
- **Patch B (D3D level):** `0x823E0B34`: `39600001` → `39600000`. Interval ONE then encodes 0 vblanks.
- Usually not needed: with `vsync=false` (1 kHz vblank), interval ONE already waits only about 1 ms. Use A or B only if Step B of section 1 still shows a cap.
- Informational: Swap's frames-ahead throttle at 0x823E14A8/0x823E14B4 spins while swaps − flips ≥ 15. It should not cap fps, but it does affect latency.

**4.2 Native 720p render (medium confidence; buffers that derive from these sizes were not checked)**
- The render size 1152x648 lives at `0x82908778` +0x30/+0x34. To get native 720p, change these immediates:
  - `39400480`→`39400500` and `39200288`→`392002D0` at 0x822501AC/0x822501B0, 0x82251380/0x82251384, 0x8225181C/0x82251820, 0x822A999C/0x822A99A8, 0x8218A570/0x8218A574;
  - `38C00480`→`38C00500` and `39000288`→`390002D0` at 0x8218A4EC/0x8218A4F0.
- These `li 0x480` hits were not checked: 0x8218B474, 0x8218B5A8, 0x82298550, 0x822985C0, 0x825F6254, 0x825F6A9C, 0x8267DF70, 0x8267E270.
- EDRAM: 1280x720 colour plus D24S8 is about 7 MB, which fits in 10 MB without MSAA. Half-res or post-FX buffers may be hard-coded.
- Ship it behind a cvar switch (the gow3 `extern "C" int XxxGamePatch(id)` post-codegen pattern), on top of `resolution_scale`.

**No widescreen/FOV, AO/DoF, aniso or draw-distance patches were verified for CA.** CA is 16:9 already. Ultrawide would need new RE of the back-buffer width and FOV, plus SDK 0026.

---

## 5. Not transferable (skip)

- Gears FPS unlock sites: gow2 0x824A2E94 / 0x8298CFB0; gow3 8253D9B0 / 82AF66BC.
- gowj UE3 `MaxSmoothedFrameRate` memory scan (`gowj_fps_unlock.cpp`).
- Gears AO/DoF upscaling fixes, black-shading fix (`GUseTilingMode`), texture-pool/streaming patches, `gow2_aniso16` (0x8280E7FC).
- Gears widescreen/Hor+ sites; gowj render-size globals (`sub_82990A28`).
- UE3 Coalesced ini edits (draw distance, shadows); CA uses bundle.data/bundle.dict.
- gowj motion-blur microcode kill. The technique, patching shaders by hash, could be reused later.
- gow2 native front end 0056-0058 (slower on PC: 151 vs 263 fps).
- Android-only patches (0007, 0015, 0016, 0052-0055, `gpu_upload_whole_blocks`) and System Link patches.
- All Vulkan flags and patches (our plugin is D3D12-only).

---

## 6. Risks and test plan

**Risks**
- **Game speed above 60 fps.** The sim looks variable-dt: dt in f1 goes to `sub_82251970`/`sub_82251FD8`, and animation is authored at 1/30 (`0x820EACFC`) but advanced by time. However, Havok stepping and any dt clamp were **not verified** (medium-low confidence). It is also unknown whether CA counts vblanks anywhere for time. A higher `video_mode_refresh_rate` or `vsync=false` could speed up anything that is vblank-counted.
- **Upscaling artifacts at scale > 1.** Watch post-FX that use fixed texel offsets (bloom, DoF, LUT), the known Xenia shield shimmer, and the chapter-3 window glitch. Scale 3 broke two Gears titles.
- **Correctness A/B flags.** `clear_memory_page_state=false` can leave stale data; `readback_memexport=false` can break particles or skinning; `occlusion_query_enable=false` can make culled objects or flares visible.
- **Tearing** without VRR (stock presenter uses interval 0). Fix with VRR, a driver cap, or patch 0021.
- **Cold-cache bias.** The NVIDIA DXCache hides stalls on repeat runs, and changing `resolution_scale` invalidates shaders.

**Test plan**
1. **Baseline.** Run the current cass.toml with the cvar dump (2.4), the FPS overlay and a CSV. Record fps, p50/p95/p99 frame time and frames over 16.7/33 ms at:
   - the title screen,
   - an outdoor area (once story mode is fixed),
   - an interior/lab level (Xenia drops to 45-55 there).

   Confirm in the dump that every key reports source = TOML, and that the "swap source is unscaled" warning does not appear.
2. **Frame rate.** Test `video_mode_refresh_rate=120/144` with `vsync=true`, then `vsync=false`, then (if still capped) item 2.1 or Patch A. At each step, check timing at 60 against the unlocked rate with a stopwatch on:
   - a fixed walk or run distance,
   - a cutscene with audio/lip sync,
   - a ragdoll or Havok object fall,
   - shield throw and return timing,
   - QTE windows,
   - climb/jump arcs,
   - menu and UI animation speed.

   Any drift means the game is frame- or vblank-locked: stay at 60 and use the limiter (2.2).
3. **Resolution.** Compare scale 2 against 3 in bloom- and DoF-heavy scenes and the shield closeup; check VRAM. Separately, try 4.2 at scale 1 first and look for EDRAM overlap or misaligned post-FX.
4. **Performance flags.** Turn on one flag at a time: `clear_memory_page_state`, `readback_memexport`, texture limits, `occlusion_query_enable`. Compare CSV percentiles and do a visual diff over 5 minutes of play each.
5. **Stutter.** Do cold runs with a new `cache_root` and a cleared DXCache. Compare the second run with 2.5 (PSO library) or a 0025-patched SDK against the first.
6. **AA.** Compare scale 2 + `fxaa`, scale 2 + host SMAA/FSR1 (2.6), and scale 3 + none. Judge HUD sharpness and edge quality.
7. **Config fix.** Correct the two wrong cass.toml comments: the "30 fps" note (it is a 60 Hz vsync-ONE cap) and the "1280x720" multiple (the base is 1152x648).