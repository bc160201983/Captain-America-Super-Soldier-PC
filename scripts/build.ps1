<#
.SYNOPSIS
  Builds cass.exe (Captain America: Super Soldier, native PC) and then, as a separate step,
  Captain America Launcher.exe next to it.

  1. Runs `rexglue codegen` on game\default.xex (writes generated\).
  2. Runs the post-codegen patchers (the cass_patch(...) lines in CMakeLists.txt) BEFORE the build
     starts. Ninja decides what to recompile when it starts, so a file a patcher rewrites in the
     middle of a build would otherwise keep its old object (this shipped a stale cass.exe once).
  3. Configures CMake (Ninja + clang from Visual Studio) and builds cass.exe, runs one catch-up
     pass, then fails if any generated source is still newer than its object.

.PARAMETER Sdk
  ReXGlue SDK folder (contains bin\rexglue.exe). Defaults to ..\tools\rexglue-sdk\win-amd64.
.PARAMETER Config
  Release (default), RelWithDebInfo or Debug.
.PARAMETER SkipCodegen
  Reuse the existing generated\ output.
#>
param(
    [string]$Sdk = $(if ($env:REXGLUE_SDK) { $env:REXGLUE_SDK } else { Join-Path $PSScriptRoot '..\..\tools\rexglue-sdk\win-amd64' }),
    [ValidateSet('Release', 'RelWithDebInfo', 'Debug')][string]$Config = 'Release',
    [switch]$SkipCodegen
)
$ErrorActionPreference = 'Stop'
function Fail($msg) { Write-Host "ERROR: $msg" -ForegroundColor Red; exit 1 }

$root = Resolve-Path (Join-Path $PSScriptRoot '..')
Set-Location $root
$Sdk = (Resolve-Path $Sdk).Path
$rexglue = Join-Path $Sdk 'bin\rexglue.exe'
if (-not (Test-Path $rexglue)) { Fail "rexglue.exe not found in $Sdk\bin." }
if (-not (Test-Path 'game\default.xex')) { Fail 'game\default.xex is missing. Extract your ISO: python scripts\extract_iso.py <iso> game' }

# Visual Studio environment (Windows SDK, MSVC libs) and its bundled clang.
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { Fail 'Visual Studio with C++ tools not found.' }
cmd /c "`"$vsPath\VC\Auxiliary\Build\vcvars64.bat`" >nul && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { Set-Item "env:$($Matches[1])" $Matches[2] }
}
$llvm = Join-Path $vsPath 'VC\Tools\Llvm\x64\bin'
if (-not (Test-Path "$llvm\clang++.exe")) { Fail 'clang++ not found. Install the VS "C++ Clang tools for Windows" component.' }

Write-Host "SDK:   $Sdk"
Write-Host "clang: $llvm"

New-Item -ItemType Directory -Force out\logs | Out-Null
if (-not $SkipCodegen) {
    Write-Host '== codegen'
    & $rexglue --log-file out\logs\codegen.log codegen cass_manifest.toml
    if ($LASTEXITCODE -ne 0) { Fail 'rexglue codegen failed (see out\logs\codegen.log).' }
}

# Post-codegen patchers, in the order CMakeLists.txt chains them (single source of truth).
# They are idempotent, so the copies Ninja runs later change nothing.
$python = (Get-Command python -ErrorAction SilentlyContinue).Source
if (-not $python) { Fail 'python not found (needed for the post-codegen patchers).' }
$patchers = Select-String -Path CMakeLists.txt -Pattern '^\s*cass_patch\(\s*\S+\s+(\S+\.py)' |
    ForEach-Object { $_.Matches[0].Groups[1].Value }
Write-Host "== patch generated code ($($patchers -join ', '))"
foreach ($p in $patchers) {
    & $python -I "tools\$p" generated\default
    if ($LASTEXITCODE -ne 0) { Fail "tools\$p failed." }
}

$build = "out\build\$($Config.ToLower())"
Write-Host '== configure'
cmake -S . -B $build -G Ninja "-DCMAKE_BUILD_TYPE=$Config" `
    "-DCMAKE_C_COMPILER=$llvm\clang.exe" "-DCMAKE_CXX_COMPILER=$llvm\clang++.exe" `
    "-DCMAKE_LINKER=$llvm\lld-link.exe" "-DCMAKE_PREFIX_PATH=$Sdk"
if ($LASTEXITCODE -ne 0) { Fail 'CMake configure failed.' }

Write-Host '== build'
cmake --build $build --target cass
if ($LASTEXITCODE -ne 0) { Fail 'Build failed.' }
# Catch-up pass: recompiles anything a patcher touched during the first pass (normally a no-op).
cmake --build $build --target cass
if ($LASTEXITCODE -ne 0) { Fail 'Build failed (catch-up pass).' }

# Never ship a stale exe: every generated source must be older than its object file.
$objDir = Join-Path $build 'CMakeFiles'
$objs = @{}
Get-ChildItem $objDir -Recurse -Filter 'cass_recomp.*.cpp.obj' | ForEach-Object { $objs[$_.Name] = $_.LastWriteTimeUtc }
$stale = Get-ChildItem generated\default -Filter 'cass_recomp.*.cpp' | Where-Object {
    $o = $objs["$($_.Name).obj"]
    (-not $o) -or ($_.LastWriteTimeUtc -gt $o)
}
if ($stale) { Fail ("{0} generated source(s) are newer than their objects (e.g. {1}); cass.exe would be stale." -f @($stale).Count, @($stale)[0].Name) }
Write-Host "Verified: all $($objs.Count) recompiled objects are up to date."
# The launcher is its own target, so an open launcher window (its exe cannot be overwritten)
# never breaks the game build.
cmake --build $build --target cass_launcher
if ($LASTEXITCODE -ne 0) {
    Write-Warning 'cass.exe is built, but Captain America Launcher.exe could not be linked (close the launcher and run this again).'
} else {
    Write-Host "Done: $build\cass.exe and $build\Captain America Launcher.exe" -ForegroundColor Green
}
