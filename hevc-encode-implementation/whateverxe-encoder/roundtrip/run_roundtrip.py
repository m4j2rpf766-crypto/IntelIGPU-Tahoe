#!/usr/bin/env python3
"""Offline round-trip check for worker HEVC serialization and transport samples."""

import argparse
from pathlib import Path
import subprocess
import tempfile


HERE = Path(__file__).resolve().parent
ENCODER = HERE.parent


def run(*args: object) -> None:
    subprocess.run([str(argument) for argument in args], check=True)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--video",
        type=Path,
        required=True,
        help="MP4 or MOV containing a compressed HEVC video track",
    )
    args = parser.parse_args()
    video = args.video.resolve(strict=True)
    for source in (ENCODER / "worker.m", ENCODER / "transport.m",
                   ENCODER / "pixel_input.m", ENCODER / "native_backend_unavailable.m"):
        if not source.is_file():
            raise SystemExit(f"required source is missing: {source}")

    frameworks = [
        "-framework", "Foundation",
        "-framework", "AVFoundation",
        "-framework", "VideoToolbox",
        "-framework", "CoreMedia",
        "-framework", "CoreVideo",
        "-framework", "IOKit",
        "-framework", "IOSurface",
        "-framework", "Accelerate",
    ]
    common = [
        "xcrun", "clang", "-arch", "x86_64",
        "-Wall", "-Wextra", "-Werror", "-Wno-deprecated-declarations",
        "-fobjc-arc", "-I", ENCODER,
    ]

    with tempfile.TemporaryDirectory(prefix="wex-sample-roundtrip-") as temporary:
        output = Path(temporary)
        packer = output / "pack_worker"
        run(
            *common, HERE / "pack_worker.m",
            ENCODER / "native_backend_unavailable.m",
            ENCODER / "pixel_input.m", *frameworks,
            "-o", packer,
        )
        records = output / "records.bin"
        run(packer, video, records)

        unpacker = output / "unpack_compare"
        run(
            *common, HERE / "unpack_compare.m",
            ENCODER / "pixel_input.m", *frameworks,
            "-o", unpacker,
        )
        run(unpacker, video, records)


if __name__ == "__main__":
    main()
