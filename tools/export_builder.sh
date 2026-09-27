#!/bin/bash
# Refresh the public Sonic-2-PS1-ISO-Builder (docs/30 phase 11) from this port, as Sonic CD's: the prebuilt executable,
# the build-time converters tools/build_assets.py runs, and the RSDKv4 sources + PS1 headers the host manifest tool
# (tools/rsdkmanifest) compiles on the user's machine. No game data. The builder's own files (build_iso.py, README.md,
# LICENSE.md, licenses/, requirements.txt) live in its repository and are not touched here.
# The executable must be the natural game (`make`, not a BOOT= / PROFILE / MEMCARD_TEST / PLATFORM_TEST build).
# Usage: tools/export_builder.sh DEST   (e.g. ../Sonic-2-PS1-ISO-Builder)
set -e
DEST=${1:?usage: tools/export_builder.sh DEST}
mkdir -p "$DEST" && DEST=$(cd "$DEST" && pwd)
cd "$(dirname "$0")/.."
nm() { mipsel-none-elf-nm rsdkv4.elf; }
if nm | grep -q g_ps1BootBuild; then echo "rsdkv4.ps-exe is a BOOT= test build: run make first"; exit 1; fi
if nm | grep -q g_ps1McTestSteps; then echo "rsdkv4.ps-exe is a MEMCARD_TEST build: run make first"; exit 1; fi
if ! nm | grep -q g_ps1InitDone; then echo "rsdkv4.ps-exe is not the game (PLATFORM_TEST?): run make first"; exit 1; fi
if ! grep -q "^PROFILE= " build/flags.txt; then echo "rsdkv4.ps-exe is a PROFILE build: run make first"; exit 1; fi
if ! grep -q "RSDK_AUTOBUILD=1" Makefile; then echo "the executable must be built with RSDK_AUTOBUILD=1 (LICENSE.md)"; exit 1; fi
B=$DEST/builder
rm -rf "$B"
mkdir -p "$B/tools/rsdkmanifest" "$B/RSDKv4/NativeObjects" "$B/ps1" "$DEST/bin"
for f in build_assets.py extract_rsdk.py bytecode_scan.py rsdkv4_filelist.txt; do cp tools/$f "$B/tools/"; done
for d in scripts atlas tiles bgstrips menu audio disc; do
  mkdir -p "$B/tools/$d"
  cp tools/$d/*.py "$B/tools/$d/"
done
cp tools/disc/boot_trace.txt "$B/tools/disc/"
rm -f "$B/tools/audio/listen_sfx.py" # a listening aid for the port, not a build step
cp tools/rsdkmanifest/Makefile tools/rsdkmanifest/main.cpp tools/rsdkmanifest/host_stubs.cpp "$B/tools/rsdkmanifest/"
cp RSDKv4/*.hpp "$B/RSDKv4/"
cp RSDKv4/NativeObjects/*.hpp "$B/RSDKv4/NativeObjects/"
for f in Scene Script Object Animation Reader String Math Sprite; do cp RSDKv4/$f.cpp "$B/RSDKv4/"; done
cp ps1/*.hh "$B/ps1/"
cp LICENSE.md "$B/RSDKv4/LICENSE.md" # the decompilation's license travels with its sources
cp rsdkv4.ps-exe "$DEST/bin/Sonic2-PS1.exe"
( cd "$DEST/bin" && shasum -a 256 Sonic2-PS1.exe > SHA256SUMS )
printf '# bin/\n\n`Sonic2-PS1.exe`: the PS1 executable, built from the RSDKv4-ps1 port at commit `%s` (`make`, with\n`RSDK_AUTOBUILD=1`). SHA-256 in `SHA256SUMS`.\n' \
  "$(git rev-parse --short HEAD)" > "$DEST/bin/README.md"
echo "exported $(git rev-parse --short HEAD) -> $DEST ($(find "$B" -type f | wc -l | tr -d ' ') builder files)"
cat "$DEST/bin/SHA256SUMS"
