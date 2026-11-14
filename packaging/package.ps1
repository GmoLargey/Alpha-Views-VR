# Builds Alpha Views VR and makes the release zips in dist\:
#   AlphaViewsVR-<version>.zip         the app, README, licence, example pictures
#   AlphaViewsVR-<version>-source.zip  the source (GPL-3.0)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$version = [regex]::Match((Get-Content "$root\CMakeLists.txt" -Raw), 'project\(AlphaViewsVR VERSION ([0-9.]+)').Groups[1].Value
if (-not $version) { throw "No version in CMakeLists.txt" }
Write-Host "Alpha Views VR $version"

cmake -S $root -B "$root\build" -A x64 | Out-Null
cmake --build "$root\build" --config Release
if ($LASTEXITCODE -ne 0) { throw "Build failed" }
$bin = "$root\build\Release"

$dist = "$root\dist"
$name = "AlphaViewsVR-$version"
$out = "$dist\$name"
if (Test-Path $out) { Remove-Item -Recurse -Force $out }
New-Item -ItemType Directory -Force $out | Out-Null

Copy-Item "$bin\AlphaViewsVR.exe", "$bin\openvr_api.dll", "$bin\alphaviewsvr.vrmanifest" $out
Copy-Item "$PSScriptRoot\LICENSE.txt", "$PSScriptRoot\THIRD_PARTY_NOTICES.txt" $out
(Get-Content "$PSScriptRoot\README.txt" -Raw).Replace('@VERSION@', $version) | Set-Content "$out\README.txt" -Encoding utf8 -NoNewline

# The example pictures: the app draws its guide panel and dashboard panel into PNGs.
$p = Start-Process -FilePath "$bin\AlphaViewsVR.exe" -ArgumentList @('--export-pictures', "`"$out`"") -Wait -PassThru
if ($p.ExitCode -ne 0 -or -not (Test-Path "$out\Guide.png") -or -not (Test-Path "$out\Dashboard.png")) { throw "Couldn't make the example pictures" }

$zip = "$dist\$name.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path $out -DestinationPath $zip

# Source package.
$src = "$dist\$name-source"
if (Test-Path $src) { Remove-Item -Recurse -Force $src }
New-Item -ItemType Directory -Force "$src\packaging" | Out-Null
Copy-Item "$root\main.cpp", "$root\CMakeLists.txt", "$root\alphaviewsvr.vrmanifest" $src
Copy-Item "$PSScriptRoot\README.txt", "$PSScriptRoot\LICENSE.txt", "$PSScriptRoot\THIRD_PARTY_NOTICES.txt", "$PSScriptRoot\package.ps1" "$src\packaging"
Copy-Item "$PSScriptRoot\LICENSE.txt" "$src\LICENSE.txt"
@"
Alpha Views VR $version - source code
https://github.com/GmoLargey/Alpha-Views-VR
Copyright (C) 2026 GmoLargey. GNU GPL version 3 (GPL-3.0-only): see LICENSE.txt.

Build (Windows, Visual Studio 2022 with C++ and CMake 3.20+):
  cmake -S . -B build -A x64
  cmake --build build --config Release
The OpenVR SDK files (openvr.h, openvr_api.lib / .dll, v2.15.6) are downloaded
from Valve's GitHub repository into build\openvr by CMake.

Release package: powershell -ExecutionPolicy Bypass -File packaging\package.ps1
"@ | Set-Content "$src\README.txt" -Encoding utf8
$szip = "$dist\$name-source.zip"
if (Test-Path $szip) { Remove-Item -Force $szip }
Compress-Archive -Path $src -DestinationPath $szip

Write-Host "Made $zip"
Write-Host "Made $szip"
