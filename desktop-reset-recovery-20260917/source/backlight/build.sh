#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"

sdk=$(xcrun --sdk macosx --show-sdk-path)
headers="$sdk/System/Library/Frameworks/Kernel.framework/Headers"
contents=build/ReimsADLBacklight.kext/Contents
mkdir -p "$contents/MacOS"

for source in ReimsADLBacklight module; do
    xcrun clang++ -O2 -g -arch x86_64 -mmacosx-version-min=13.0 -std=gnu++17 \
        -mkernel -fapple-kext -fno-builtin -fno-exceptions -fno-rtti \
        -fno-stack-protector -nostdinc++ -DKERNEL -DKERNEL_PRIVATE \
        -isysroot "$sdk" -isystem "$headers" -Wall -Wextra -Werror \
        -Wno-error=deprecated-declarations -c "$source.cpp" -o "build/$source.o"
done

xcrun clang++ -O2 -g -arch x86_64 -mmacosx-version-min=13.0 -nostdlib \
    -isysroot "$sdk" -Wl,-kext -Wl,-undefined,dynamic_lookup \
    build/ReimsADLBacklight.o build/module.o -lkmod -lkmodc++ \
    -o "$contents/MacOS/ReimsADLBacklight"
cp Info.plist "$contents/Info.plist"
plutil -lint "$contents/Info.plist"
file "$contents/MacOS/ReimsADLBacklight"
