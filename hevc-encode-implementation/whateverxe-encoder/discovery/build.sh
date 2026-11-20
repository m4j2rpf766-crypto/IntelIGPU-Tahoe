#!/bin/bash
set -euo pipefail

cd "$(dirname "$0")"
sdk=$(xcrun --sdk macosx --show-sdk-path)
headers="$sdk/System/Library/Frameworks/Kernel.framework/Headers"
build_root=${WEX_METAL_ROUTE_BUILD_ROOT:-${TMPDIR:-/tmp}/WEXIntelHEVCMetalRoute-build}
bundle="$build_root/WEXIntelHEVCMetalRoute.kext"
contents="$bundle/Contents"
mkdir -p "$contents/MacOS"

for source in route module; do
	xcrun clang++ -O2 -arch x86_64 -mmacosx-version-min=13.0 \
		-std=gnu++17 -mkernel -fapple-kext -fno-builtin -fno-exceptions \
		-fno-rtti -fno-stack-protector -nostdinc++ -DKERNEL -DKERNEL_PRIVATE \
		-isysroot "$sdk" -isystem "$headers" -Wall -Wextra -Werror \
		-Wno-error=deprecated-declarations -c "$source.cpp" -o "$build_root/$source.o"
done

xcrun clang++ -arch x86_64 -mmacosx-version-min=13.0 -nostdlib \
	-isysroot "$sdk" -Wl,-kext -Wl,-undefined,dynamic_lookup \
	"$build_root/route.o" "$build_root/module.o" -lkmod -lkmodc++ \
	-o "$contents/MacOS/WEXIntelHEVCMetalRoute"
cp Info.plist "$contents/Info.plist"
xattr -cr "$bundle"
codesign --force --sign - "$bundle"
codesign --verify --deep --strict "$bundle"

echo "Built $bundle"
