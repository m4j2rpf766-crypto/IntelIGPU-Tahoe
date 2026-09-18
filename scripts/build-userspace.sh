#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build
clang -bundle -fno-objc-arc -Wall -Wextra -Werror -undefined dynamic_lookup -framework Foundation -framework Metal hevc-encode-implementation/backend/service-route-20260917/factory.m -o build/ReimsTahoeMetalDevice
clang -framework CoreFoundation -framework IOKit desktop-manual-20260917/fix/manual-gate/control.c -o desktop-manual-20260917/fix/manual-gate/control
clang -framework Foundation -framework VideoToolbox -framework CoreMedia -framework CoreVideo -I hevc-encode-implementation/backend hevc-encode-implementation/backend/service-route-20260917/vt_client.m -o build/vt_client
