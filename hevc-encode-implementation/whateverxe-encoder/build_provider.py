#!/usr/bin/env python3
"""Build an isolated Intel VideoToolbox provider and its verification client."""
import argparse
from pathlib import Path
import shutil
import subprocess

HERE = Path(__file__).resolve().parent


def run(*args):
    subprocess.run([str(arg) for arg in args], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--worker", required=True, type=Path,
                        help="qualified, signed Intel HEVC worker built for this OS and driver")
    parser.add_argument("--out", type=Path, default=HERE / "build")
    args = parser.parse_args()
    worker = args.worker.resolve(strict=True)
    run("codesign", "--verify", "--strict", worker)
    run("lipo", worker, "-verify_arch", "x86_64")
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    helper = out / "WEXHEVCBackend/Contents/MacOS/vt_client"
    helper.parent.mkdir(parents=True, exist_ok=True)
    if helper.resolve() != worker:
        shutil.copy2(worker, helper)
    run("codesign", "--verify", "--strict", helper)
    common = ["xcrun", "clang", "-arch", "x86_64", "-Wall", "-Wextra", "-Werror", "-fobjc-arc"]
    frameworks = ["-framework", "Foundation", "-framework", "VideoToolbox", "-framework", "CoreMedia",
                  "-framework", "CoreVideo", "-framework", "IOKit", "-framework", "IOSurface",
                  "-framework", "Accelerate"]
    library = out / "libWEXIntelHEVC.dylib"
    run(*common, "-dynamiclib", "-install_name", "@rpath/" + library.name,
        HERE / "provider.m", HERE / "transport.m", HERE / "pixel_input.m", *frameworks, "-o", library)
    run("codesign", "--force", "--sign", "-", library)
    client = out / "check_provider"
    run(*common, HERE / "check_provider.m", "-L", out, "-lWEXIntelHEVC", "-Wl,-rpath,@loader_path",
        *frameworks, "-o", client)
    run("codesign", "--force", "--sign", "-", client)
    for path in (library, client):
        run("codesign", "--verify", "--strict", path)
    print(client)


if __name__ == "__main__":
    main()
