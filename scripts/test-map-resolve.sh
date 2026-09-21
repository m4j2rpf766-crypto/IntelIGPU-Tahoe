#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/map-resolve-checks
clang -dynamiclib -fobjc-arc -Wall -Wextra -Werror -framework Foundation -framework Metal \
  mapkit-resolve-compat/resolve_compat.m -o build/map-resolve-checks/libReimsMapResolve.dylib
clang -dynamiclib -Wall -Wextra -Werror mapkit-resolve-compat/checks/install.c \
  -o build/map-resolve-checks/install.dylib
swiftc mapkit-resolve-compat/checks/msaa_probe.swift -o build/map-resolve-checks/msaa_probe
for mode in separate-pass same-pass single; do
  REIMS_SYNC_DYLIB="$PWD/build/map-resolve-checks/install.dylib" \
  REIMS_TEST_LIBRARY="$PWD/build/map-resolve-checks/libReimsMapResolve.dylib" \
    build/map-resolve-checks/msaa_probe "$mode" | tee "build/map-resolve-checks/$mode.log"
done
