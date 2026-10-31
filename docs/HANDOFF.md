# Handoff — 2026-10-31

Status of the Captain America: Super Soldier PC port, and exactly where the open work stopped.
Resume from the **Open problem** section.

## Where things stand

| | |
|---|---|
| Repo | private, `bc160201983/Captain-America-Super-Soldier-PC`, branch `main`, all work pushed |
| Releases | v0.1.0–v0.1.3 published (v0.1.2 marked "replaced by v0.1.3"). **v0.1.4 is a DRAFT** (hidden) on purpose: the user wants the native-720p ghost fixed before releasing |
| Game on the user's PC | `out/build/release` (desktop shortcut → `Captain America Launcher.exe`). Plays fine at **1296p** and **~4K** (no native 720p) |
| User's save | `C:\Users\bilal\Documents\cass\B13EBABEBABEBABE\...` (slot "Stand Alone Together", Chapter 1) |

### Done and verified
- Builds, boots, plays; runs from the extracted folder **or straight from the ISO**.
- 176 + 176 missed functions harvested (`tools/harvest_hints.py`, incl. the "gap" scan).
- Guest `trap` → log and continue (`tools/patch_guest_traps.py`); 243 jump-table defaults → follow CTR or loud fatal (`tools/patch_switch_traps.py`); crash guard writes `logs/cass_crash_*.txt/.dmp` + dialog.
- `scripts/build.ps1` runs all post-codegen patchers **before** Ninja, does a catch-up pass and refuses stale objects (v0.1.2 shipped stale because of this).
- **Native 720p load freeze fixed**: the Permanent arena (0x82A0E974, 18.75 MiB) overflowed on level load; `patch_native_720p.py` now routes the two arena-size sites (0x822B5984, 0x822B59A4) through `CassRenderArenaSize` → 21.19 MiB with native 720p. **User verified**: the save loads and plays at 1440p.
- SMAA + FSR1, host vsync (`cass_vsync`), frame pacing, PSO library, launcher with resolution presets, scripted test input (`CASS_TEST_INPUT`, inert unless set).

## Open problem: native-720p "ghost"

**Symptom.** With `cass_native_720p = true` (launcher presets **1440p** and **4K 3840x2160**), a faint copy of
Cap appears **shrunk toward the top-left at ~0.9×** (0.9 = 1152/1280 = 648/720): dark on the floor in
gameplay, orange in the pause menu, and on walls it is clearly **his shadow silhouette** (head and
shoulders) in the wrong place. Gone with native 720p off (user confirmed). Screenshots from the user
are in this session's images (pause menu, floor, wall).

**Interpretation.** Some pass still maps screen positions with the old 1152×648 size while the scene is
1280×720, so its content lands at 0.9× position. The wall shot points at the **deferred / screen-space
shadow** path: pass `"SS Shadow Resolve"` (string 0x820F7ED8, node created at 0x82231788 in the
function around 0x822316C8; it renders into a target set from the registry at **0x829016BC +0xC**, which is
filled by the half-size / quarter-size target-set functions sub_8225AC88 / sub_8225ADC8). Related names:
`ShadowsResolve Root` (0x82250A38), `DeferredLightingShadows/ShadowParam` (0x82109278, not referenced by
lis/addi — probably looked up by hash), `ShadowColorTex`, `ShadowTex`, `ShadowIndexMask`,
`DShadowStrengths`, `DeferredLighting/LBuffers`, `GBuffer`, and the TriOviz library (`DwTrioviz*`,
code ~0x8282C000–0x82837000, shader binding helpers ~0x8232D000–0x8233D000).

### Ruled out (do not redo)
- **Every literal form of the old size in the XEX**: `li 1152/648` (all non-patched `li 0x480` are memset/memcpy sizes:
  0x8218B474, 0x8218B5A8, 0x82298550, 0x822985C0, 0x825F6254, 0x825F6A9C, 0x8267DF70, 0x8267E270);
  `li 576/324/288/162/640/360` pairs; inclusive 1151/647; packed `(h<<16)|w` register values; 16/32-bit
  pairs in data; doubles; floats 1/1152, 1/648, 0.5/1152, 1/576, 1/324 (none exist).
- **The 1152.0/648.0 float pair** (0x820EB0B4/B8, pool base 0x820EACD0 +996/+1000): its **only two loads**
  are the already-patched ortho sites 0x822513C8/0x822513CC.
- **×0.9 size math** (no code loads 0.9f and converts to an integer size).
- **1280×720 `li` pairs** at 0x82237238 and 0x8282D954: display-mode / TriOviz default tables (with 853×480).
- **Render targets**: Main Clr/Depth (patched), **Main Alpha Target** (copies Main Clr's descriptor, memcpy at
  0x82250300), half/quarter sets (sizes copied from an existing target descriptor, `lhz 4/6`),
  frontbuffer/Color, Color2, depth (sub_822B9E28 — sizes from virtual getters = patched config),
  sub_8282EFC0 post-FX textures (divided from passed-in values).
- **Screen-space point mapping** sub_8218A4B8 (copy at 0x82A0BE5C): consistent under native 720p.
- **EDRAM rebase** of half/quarter depth bases: user tested `cass_native_720p_edram_rebase = false` →
  ghost still there (setting restored to default afterwards).
- Shader literals in `DATA.0–2`: archives are compressed (XctdCompression), scan inconclusive.

### Next steps (suggested)
1. The size is not a literal: find where the **shadow / lighting shader constants** are computed. Trace who
   writes the shader-parameter blocks for the SS-shadow / deferred-lighting materials (the material system
   looks parameters up by name or hash — find the hash function and the hash of
   `DeferredLightingShadows/ShadowParam`, then its writers), and check whether any value is derived from a
   **texture/surface descriptor of a 1152×648-sized object created before the patched sites run**, or from a
   **viewport/scissor rect** stored in a camera or view object.
2. Dynamic option (needs the user to run it; do **not** start test games on the user's PC without asking —
   the user plays on it and two game copies at once break loading): a cvar-gated diagnostic that logs
   viewport/scissor/resolve/texture sizes equal to 1152/648/576/324 with the guest function + lr, then the user
   loads the save at 1440p and sends the log.
3. Patch the found site through `tools/patch_native_720p.py` + `src/cass_render_size.cpp` (native off must stay
   bit-identical), build with `scripts\build.ps1 -SkipCodegen`, have the user test at 1440p, then publish the
   **v0.1.4 draft** (or a new version) — release only after the user confirms.

## Other known issues
- **Random startup crash** (rare): in the first second of boot, `game:\materials\material_new.dict` is
  requested and the game crashes at sub_82323DC8+0x293 (null read after an assert at lr 0x82323E50). It means
  the archive lookup through sub_822C72F8 failed. Seen in logs cass_024 (16:47:00), cass_027, cass_028; the next
  start usually works. Not investigated; probably a timing race at boot.
- `NtQueryInformationFile(XFileXctdCompressionInformation) unimplemented` errors in every log (harmless so far).
- OBS **Game Capture** cannot hook (our D3D12 Present hooks); use Window Capture (WGC).

## How the user wants to work
- **One agent at most** (no parallel workflows), and preferably none: do the work directly.
- **Do not start test game windows** on the user's PC; tell the user what to test and they test.
- **Fix first, then release** — never publish before the user confirms.
- Keep a save backup before any test that loads the save.

## Useful commands
```powershell
# build (runs patchers first, refuses stale objects)
powershell -ExecutionPolicy Bypass -File scripts\build.ps1 -SkipCodegen
# package a release zip (out\package\...)
powershell -ExecutionPolicy Bypass -File scripts\package.ps1 -Version 0.1.4
# publish the draft after the user confirms
gh release edit v0.1.4 --repo bc160201983/Captain-America-Super-Soldier-PC --draft=false
```
Decrypted image for analysis: `python -I tools\dump_xex_image.py game\default.xex out\image`
(`out/image/image.bin` at 0x82000000; capstone PPC big-endian is installed).
