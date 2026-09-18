#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
sdk=$(xcrun --sdk macosx --show-sdk-path)
headers="$sdk/System/Library/Frameworks/Kernel.framework/Headers"
out=build/ReimsVideoDiscovery.kext/Contents
mkdir -p "$out/MacOS"
for src in publisher module; do
 xcrun clang++ -O2 -arch x86_64 -mmacosx-version-min=13.0 -std=gnu++17 -mkernel -fapple-kext -fno-builtin -fno-exceptions -fno-rtti -fno-stack-protector -nostdinc++ -DKERNEL -DKERNEL_PRIVATE -isysroot "$sdk" -isystem "$headers" -Wall -Wextra -Werror -Wno-error=deprecated-declarations -c "$src.cpp" -o "build/$src.o"
done
xcrun clang++ -arch x86_64 -mmacosx-version-min=13.0 -nostdlib -isysroot "$sdk" -Wl,-kext -Wl,-undefined,dynamic_lookup build/publisher.o build/module.o -lkmod -lkmodc++ -o "$out/MacOS/ReimsVideoDiscovery"
cp Info.plist "$out/Info.plist"

codesign --force --sign - build/ReimsVideoDiscovery.kext
codesign --verify --deep --strict build/ReimsVideoDiscovery.kext
python3 ../../scripts/write-kext-receipt.py \
 build/ReimsVideoDiscovery.kext ReimsVideoDiscovery video-discovery-current.json
