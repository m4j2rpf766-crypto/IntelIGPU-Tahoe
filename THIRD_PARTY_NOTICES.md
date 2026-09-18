# Third-party notices

Original project source is licensed under GPL-3.0-only. This does not replace licenses embedded in third-party files.

## Intel media-driver

Upstream: https://github.com/intel/media-driver

Source reference commit: `fba6a82db8cb02f11d4b360c73dc39ab730c8bf4`.

The following files contain Intel MIT-licensed definitions; retain their copyright and permission notices:

- `hevc-encode-implementation/backend/official-reference/media_driver/agnostic/gen12/hw/vdbox/mhw_vdbox_vdenc_hwcmd_g12_X.h`
- `hevc-encode-implementation/backend/official-reference/media_driver/agnostic/gen12/hw/vdbox/mhw_vdbox_vdenc_hwcmd_g12_X.cpp`
- `hevc-encode-implementation/backend/official-reference/media_driver/agnostic/gen12/hw/vdbox/mhw_vdbox_hcp_hwcmd_g12_X.h`
- `hevc-encode-implementation/backend/official-reference/media_driver/agnostic/gen12/hw/vdbox/mhw_vdbox_hcp_hwcmd_g12_X.cpp`
- `hevc-encode-implementation/backend/command-catalog/gen12_rdoq_tables.h` (extracted tables; notice preserved).

The minimal `command-catalog/mos_utilities.h` is a local compatibility shim. Apple SDKs and runtime components are external dependencies governed by their own terms; no license to them is granted here. This project is not affiliated with Apple or Intel.
