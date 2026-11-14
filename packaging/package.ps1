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
New-Item -ItemType Directory -Force "$src\packaging", "$src\linux" | Out-Null
Copy-Item "$root\main.cpp", "$root\CMakeLists.txt", "$root\alphaviewsvr.vrmanifest" $src
Copy-Item "$root\linux\sal.h" "$src\linux"
Copy-Item "$PSScriptRoot\README.txt", "$PSScriptRoot\LICENSE.txt", "$PSScriptRoot\THIRD_PARTY_NOTICES.txt", "$PSScriptRoot\package.ps1", "$PSScriptRoot\package.sh" "$src\packaging"
Copy-Item "$PSScriptRoot\LICENSE.txt" "$src\LICENSE.txt"
@"
Alpha Views VR $version - source code
https://github.com/GmoLargey/Alpha-Views-VR
Copyright (C) 2026 GmoLargey. GNU GPL version 3 (GPL-3.0-only): see LICENSE.txt.

Build on Windows (Visual Studio 2022 with C++, CMake 3.20+):
  cmake -S . -B build -A x64
  cmake --build build --config Release

Build on Linux (x86-64; g++ 9+ or clang, CMake 3.20+, curl/TLS for the downloads):
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
  cmake --build build -j

CMake downloads the SDK files into build\: OpenVR (v2.15.6), Vulkan-Headers,
stb_truetype / stb_image_write, and on Linux DirectXMath. The Vulkan loader is
the one on your computer (it comes with the graphics driver).

Release packages: packaging\package.ps1 (Windows), packaging/package.sh (Linux).
"@ | Set-Content "$src\README.txt" -Encoding utf8
$szip = "$dist\$name-source.zip"
if (Test-Path $szip) { Remove-Item -Force $szip }
# Windows' tar: standard "/" paths, so the source zip also unpacks properly on Linux.
& "$env:SystemRoot\System32\tar.exe" -a -c -f $szip -C $dist "$name-source"
if ($LASTEXITCODE) { throw "Source zip failed" }

Write-Host "Made $zip"
Write-Host "Made $szip"
