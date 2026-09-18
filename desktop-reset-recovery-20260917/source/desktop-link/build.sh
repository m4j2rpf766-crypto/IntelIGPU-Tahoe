#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"

# Validate private map getter slots against the binary this kext accompanies.
python3 ../tools/verify_tgl_map_abi.py "${REIMS_TGL_IMAGE:?Set REIMS_TGL_IMAGE to your locally obtained, ABI-compatible TGL kernel image}"

sdk=$(xcrun --sdk macosx --show-sdk-path)
headers="$sdk/System/Library/Frameworks/Kernel.framework/Headers"
contents=build/ReimsADLDesktopLink.kext/Contents
mkdir -p "$contents/MacOS"

for source in ReimsADLDesktopLink graphics_control intel_framebuffer module; do
    xcrun clang++ -O2 -g -arch x86_64 -mmacosx-version-min=13.0 -std=gnu++17 \
        -mkernel -fapple-kext -fno-builtin -fno-exceptions -fno-rtti \
        -fno-stack-protector -nostdinc++ -DKERNEL -DKERNEL_PRIVATE \
        -isysroot "$sdk" -isystem "$headers" -Wall -Wextra -Werror \
        -Wno-error=deprecated-declarations -c "$source.cpp" -o "build/$source.o"
done

xcrun clang++ -O2 -g -arch x86_64 -mmacosx-version-min=13.0 -nostdlib \
    -isysroot "$sdk" -Wl,-kext -Wl,-undefined,dynamic_lookup \
    build/ReimsADLDesktopLink.o build/graphics_control.o build/intel_framebuffer.o build/module.o -lkmod -lkmodc++ \
    -o "$contents/MacOS/ReimsADLDesktopLink"
cp Info.plist "$contents/Info.plist"
codesign --force --sign - build/ReimsADLDesktopLink.kext
codesign --verify --deep --strict build/ReimsADLDesktopLink.kext
plutil -lint "$contents/Info.plist"
file "$contents/MacOS/ReimsADLDesktopLink"
python3 ../../../scripts/write-kext-receipt.py \
 build/ReimsADLDesktopLink.kext ReimsADLDesktopLink desktop-link-current.json
