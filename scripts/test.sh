#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
python3 hevc-encode-implementation/backend/error-recovery-20260918/test.py
python3 desktop-manual-20260917/fix/runtime-video-20260918/test_runtime_video.py
(cd desktop-reset-recovery-20260917/collector-v5 && python3 test_events.py && python3 test_store.py)
