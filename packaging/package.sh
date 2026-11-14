#!/bin/sh
# Builds Alpha Views VR for Linux (x86-64) and makes dist/AlphaViewsVR-<version>-linux.tar.gz:
# the program, libopenvr_api.so, the SteamVR manifest, README, licence and example pictures.
# The Windows packages (and the source zip) come from package.ps1.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
version=$(sed -n 's/^project(AlphaViewsVR VERSION \([0-9.]*\).*/\1/p' "$root/CMakeLists.txt")
[ -n "$version" ] || { echo "No version in CMakeLists.txt"; exit 1; }
echo "Alpha Views VR $version (Linux)"

build=${BUILD_DIR:-$root/build-linux}
cmake -S "$root" -B "$build" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$build" -j

name="AlphaViewsVR-$version-linux"
out="$root/dist/$name"
rm -rf "$out"
mkdir -p "$out"
cp "$build/AlphaViewsVR" "$build/libopenvr_api.so" "$build/alphaviewsvr.vrmanifest" "$out/"
cp "$root/packaging/LICENSE.txt" "$root/packaging/THIRD_PARTY_NOTICES.txt" "$out/"
sed "s/@VERSION@/$version/g" "$root/packaging/README.txt" > "$out/README.txt"
"$out/AlphaViewsVR" --export-pictures "$out"
# Pack from a staging copy with plain permissions (a Windows drive under WSL makes everything 777).
stage=$(mktemp -d)
cp -r "$out" "$stage/"
find "$stage/$name" -type d -exec chmod 755 {} +
find "$stage/$name" -type f -exec chmod 644 {} +
chmod 755 "$stage/$name/AlphaViewsVR" "$stage/$name/libopenvr_api.so"
tar -C "$stage" --owner=0 --group=0 --numeric-owner -czf "$root/dist/$name.tar.gz" "$name"
rm -rf "$stage"
echo "Made $root/dist/$name.tar.gz"
# The program needs this glibc or newer (the one it was built with):
objdump -T "$out/AlphaViewsVR" 2>/dev/null | grep -o 'GLIBC_[0-9.]*' | sort -Vu | tail -1 || true
