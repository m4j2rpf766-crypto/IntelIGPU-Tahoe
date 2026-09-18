# Source map

Historical directory names are preserved because the Metal entry source includes the compatibility layers by relative path.

| Directory | Purpose |
| --- | --- |
| desktop-reset-recovery-20260917/source | Current DesktopLink, backlight, ABI checks |
| desktop-reset-recovery-20260917/collector-v5 | Incident recorder and synthetic tests |
| desktop-manual-20260917/fix | Explicit activation controller and runtime checks |
| video-decode-candidate/manual-runtime-discovery-20260917 | Video capability publication |
| wechat-menu-tearing/compat-source | Metal entry and compatibility layer |
| app-blank-candidate, jianying-startup-candidate, cpu-performance-candidate | Included Metal compatibility sources |
| hevc-encode-implementation/backend | HEVC backend, service bridge, validators, error recovery |

This is a curated source export, not a copy of the private diagnostic workspace or its history. Excluded: Apple binaries, all built artifacts, raw captures/logs, EFI data, unrelated wireless/remote-management experiments and host-specific installation scripts. Public adaptations remove private paths/fixtures, require a local ABI input, and use synthetic recorder fixtures. Runtime identity checks are retained.
