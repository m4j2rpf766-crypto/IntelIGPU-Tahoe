#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
test -f profiles.hpp
test -f deferred_uuid.hpp
sdk=$(xcrun --sdk macosx --show-sdk-path)
headers="$sdk/System/Library/Frameworks/Kernel.framework/Headers"
contents=build/ReimsADLManualActivation.kext/Contents
mkdir -p "$contents/MacOS"
for source in gate module; do
 xcrun clang++ -O2 -g -arch x86_64 -mmacosx-version-min=13.0 -std=gnu++17 \
 -mkernel -fapple-kext -fno-builtin -fno-exceptions -fno-rtti -fno-stack-protector \
 -nostdinc++ -DKERNEL -DKERNEL_PRIVATE -isysroot "$sdk" -isystem "$headers" \
 -Wall -Wextra -Werror -Wno-error=deprecated-declarations \
 -c "$source.cpp" -o "build/$source.o"
done
xcrun clang++ -arch x86_64 -mmacosx-version-min=13.0 -nostdlib -isysroot "$sdk" \
 -Wl,-kext -Wl,-undefined,dynamic_lookup build/gate.o build/module.o -lkmod -lkmodc++ \
 -o "$contents/MacOS/ReimsADLManualActivation"
cp Info.plist "$contents/Info.plist"
codesign --force --sign - build/ReimsADLManualActivation.kext
codesign --verify --deep --strict build/ReimsADLManualActivation.kext
plutil -lint "$contents/Info.plist"
python3 ../../../scripts/write-kext-receipt.py \
 build/ReimsADLManualActivation.kext ReimsADLManualActivation ../manual-gate-current.json
