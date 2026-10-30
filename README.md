# Captain America: Super Soldier — native PC (work in progress)

Static recompilation of the Xbox 360 game to native x64 with the
[ReXGlue SDK](https://github.com/rexglue/rexglue-sdk). Bring your own disc; no game files here.

Starting point: [m1ndcrap/Captain-America-Super-Soldier-Recompiled](https://github.com/m1ndcrap/Captain-America-Super-Soldier-Recompiled)
(SDK 0.4: boots to the title screen, crashes entering story mode). Layout and workflow follow
[gears-of-war-2-pc-port](https://github.com/santiquiroz/gears-of-war-2-pc-port).

## Supported executable

| | |
|---|---|
| Title ID | `53450858` |
| Media ID | `74041439` |
| Version | `0.0.0.2` (base disc, no title update) |
| Entry point | `0x824A70F0` |

## Requirements

- Windows 10/11 x64, Visual Studio 2022 Build Tools with C++ and "C++ Clang tools for Windows"
- CMake 3.25+, Ninja, Python 3.9+
- ReXGlue SDK `0.10.0.9-dev` (release tag `nightly-20260921-923c1a59`, same build as the GoW2 port),
  unpacked to `..\tools\rexglue-sdk\win-amd64` or set `REXGLUE_SDK`

## Build

1. Extract your ISO into `game\`:

   ```powershell
   python scripts\extract_iso.py "<your iso>" game
   ```

2. Build:

   ```powershell
   powershell -ExecutionPolicy Bypass -File scripts\build.ps1
   ```

   Output: `out\build\release\cass.exe` and `Captain America Launcher.exe` next to it.

3. Play: start `Captain America Launcher.exe`. It finds `game\` (next to the exe, or
   `..\..\..\game` in this dev layout) or a single game `.iso` (next to the exe, in `..\..\..`, or in
   `..\..\..\..`, i.e. beside `ca-recomp\` in this dev layout), lets you pick another location (Browse: your `.iso` or `default.xex` in a folder; right-click the
   path to pick a folder), edits the common settings in `cass.toml` (only the keys it shows; every
   other line is kept), and starts the game. The location is saved as `cass_game_dir` in `cass.toml`,
   so `cass.exe` also runs without `--game_data_root`.
   Headless: `"Captain America Launcher.exe" [--config <toml>] --apply key=value ...`
   (`game_dir=` must be a folder with `default.xex` or the game `.iso`; otherwise it exits with
   code 3 and writes nothing).

### Running straight from the ISO

Extracting is only needed for codegen (step 1). To play, `cass_game_dir` (or `--cass_game_dir`,
or `--game_data_root`) may name the `.iso` itself:

```powershell
cass.exe --cass_game_dir="F:\games\Captain America - Super Soldier (USA, Europe).iso"
```

`cass.exe` copies `default.xex` out of the image once into `<cache>\iso\<name>-<size>\` (the
runtime needs a folder to start from) and then mounts the disc image itself (the SDK's XDVDFS
`DiscImageDevice`) as `game:\` and `d:\`, so everything else is read from the ISO. The log says
`cass: running from ISO ...`.

## Layout

| Path | What |
|---|---|
| `cass_manifest.toml` | codegen manifest |
| `hints/original.toml` | setjmp/longjmp, function hints and FPU hooks from the 0.4 port |
| `hints/runtime.toml` | function addresses found by booting the game |
| `src/` | host app (`cass_app.h` sets runtime options) |
| `launcher/` | `Captain America Launcher.exe`: Win32 + GDI+ settings window (from the Judgment port) |
| `scripts/extract_iso.py` | XDVDFS extractor (needed for codegen; playing can use the ISO directly) |
| `scripts/build.ps1` | codegen + CMake build |

## Fixing crashes: the hint loop

When the game reaches code that codegen did not find, the runtime stops with
`call to unregistered function 0x8XXXXXXX`. Add the address to `hints/runtime.toml`, rebuild,
and play further. Repeat until story mode loads. Missed `switch` jump tables can be recovered
with [xenon-jumptables](https://github.com/xdzleo/xenon-jumptables).

## Notes

- The game does not use Unreal Engine (assets are `bundle.data` / `bundle.dict`, plus
  `DATA.0`–`DATA.2`), so Gears engine patches don't apply; their tooling does.
