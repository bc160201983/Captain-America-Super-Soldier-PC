# Third-party notices

Captain America: Super Soldier (PC) by BilalReloaded is built on and adapts code from the projects
below. No game files are included: you need your own copy of the Xbox 360 game.

| Component | Used for | License |
|---|---|---|
| [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) (`rexruntime.dll`, `rexgpu-xenos.dll`), derived from [Xenia](https://github.com/xenia-project/xenia) | recompiler and Xbox 360 runtime | BSD 3-Clause (Copyright (c) 2015 Ben Vanik and contributors) |
| [Gears of War: Judgment recomp](https://github.com/OverkillLabs3/gears-judgement-recomp) | launcher, SMAA/FSR pass, pipeline library, frame pacing, FPS overlay, system tuning (adapted) | BSD 3-Clause (Copyright (c) 2026 the contributors to this project) |
| [Gears of War 2 PC port](https://github.com/santiquiroz/gears-of-war-2-pc-port) | project layout, hint-harvesting method, switch-trap patcher and crash watch (adapted) | BSD 3-Clause (Copyright (c) 2026 Santiago Quiroz Upegui and the gow2-recomp contributors) |
| [Gears of War 3 recomp](https://github.com/Flippage/gears-of-war-3-recomp) | host-vsync idea, launcher ideas | BSD 3-Clause (Copyright (c) 2026 Flippage and the gow3-recomp contributors) |
| [Captain-America-Super-Soldier-Recompiled](https://github.com/m1ndcrap/Captain-America-Super-Soldier-Recompiled) | original function hints and FPU hooks | (no license file) |
| [AMD FidelityFX Super Resolution 1.0](https://github.com/GPUOpen-Effects/FidelityFX-FSR) | upscaling and sharpening | MIT (Copyright (c) 2021 Advanced Micro Devices, Inc.), see `third_party/fsr1/LICENSE` |
| [SMAA](https://github.com/iryoku/smaa) | anti-aliasing | MIT (Copyright (C) 2013 Jorge Jimenez et al.), see `third_party/smaa/LICENSE` |
| SDL3, fmt, spdlog, FFmpeg (libavcodec/libavutil) and others linked into the ReXGlue runtime | input, logging, audio decoding | see the ReXGlue SDK `licenses` folder |
| Microsoft Visual C++ runtime (`msvcp140*.dll`, `vcruntime140*.dll`) | C++ runtime, redistributed app-locally | Microsoft Visual Studio redistributable license |

BSD 3-Clause terms (for the ReXGlue/Xenia and Gears-port code): redistributions must retain the
copyright notices above, this list of conditions and the disclaimer; the names of the projects and
their contributors may not be used to endorse or promote products derived from this software
without permission. THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.

Captain America and Super Soldier are trademarks of Marvel. Not affiliated with or endorsed by
Marvel, SEGA, Next Level Games or Microsoft.
