<#
.SYNOPSIS
  Packages the release zip: cass.exe, the ReXGlue runtime DLLs, Captain America Launcher.exe,
  the app-local Visual C++ runtime, default cass.toml, QUICKSTART.txt and THIRD_PARTY_NOTICES.md.
  No game files. Run scripts\build.ps1 first.

.PARAMETER Version
  Release version, e.g. 0.1.0. Output: out\package\CaptainAmerica-SuperSoldier-PC-v<Version>-win64.zip
#>
param([Parameter(Mandatory)][string]$Version)
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..')
$rel = Join-Path $root 'out\build\release'
$name = "CaptainAmerica-SuperSoldier-PC-v$Version"
$pkgRoot = Join-Path $root 'out\package'
$pkg = Join-Path $pkgRoot $name
$zip = Join-Path $pkgRoot "$name-win64.zip"

# App-local VC++ runtime from the newest Visual Studio redist folder.
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -property installationPath
$crt = Get-ChildItem "$vs\VC\Redist\MSVC\*\x64\Microsoft.VC14*.CRT" -Directory | Sort-Object FullName | Select-Object -Last 1
if (-not $crt) { throw 'Visual C++ redist folder not found.' }

if (Test-Path $pkg) { Remove-Item $pkg -Recurse -Force }
New-Item -ItemType Directory -Force $pkg | Out-Null
foreach ($f in 'cass.exe', 'rexruntime.dll', 'rexgpu-xenos.dll', 'Captain America Launcher.exe') {
    Copy-Item (Join-Path $rel $f) $pkg
}
foreach ($d in 'msvcp140.dll', 'msvcp140_atomic_wait.dll', 'vcruntime140.dll', 'vcruntime140_1.dll') {
    Copy-Item (Join-Path $crt.FullName $d) $pkg
}
Copy-Item (Join-Path $root 'THIRD_PARTY_NOTICES.md') $pkg
Copy-Item (Join-Path $root 'config\QUICKSTART.txt') $pkg

# Default settings for a new player: 60 fps cap (the launcher offers the monitor's rate), no
# game path. Written as UTF-8 without a BOM (the TOML loader and launcher expect that).
$toml = (Get-Content (Join-Path $root 'config\cass.toml') -Raw) -replace '(?m)^video_mode_refresh_rate = \d+', 'video_mode_refresh_rate = 60'
[IO.File]::WriteAllText((Join-Path $pkg 'cass.toml'), $toml, (New-Object Text.UTF8Encoding $false))

if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path $pkg -DestinationPath $zip -CompressionLevel Optimal
Write-Host ("Done: {0} ({1:N1} MB)" -f $zip, ((Get-Item $zip).Length / 1MB)) -ForegroundColor Green
