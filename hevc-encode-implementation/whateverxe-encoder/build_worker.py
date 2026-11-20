#!/usr/bin/env python3
"""Build the x86_64 HEVC IPC worker and check its no-frame readiness result."""

import argparse
import os
from pathlib import Path
import stat
import subprocess
import tempfile


HERE = Path(__file__).resolve().parent


def run(*args: object) -> None:
    subprocess.run([str(arg) for arg in args], check=True)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--out",
        type=Path,
        default=HERE / "build",
        help="safe user-owned build directory",
    )
    parser.add_argument(
        "--native-adapter",
        type=Path,
        help="source file implementing the native backend contract",
    )
    parser.add_argument(
        "--expect",
        choices=("available", "unavailable"),
        default="unavailable",
        help="outcome required by the no-frame backend probe",
    )
    args = parser.parse_args()

    raw_output_directory = args.out.expanduser()
    if raw_output_directory.is_symlink():
        raise RuntimeError(f"refusing a symlink build directory: {raw_output_directory}")
    output_directory = raw_output_directory.resolve()
    protected_roots = tuple(
        Path(path) for path in ("/System", "/Library", "/Applications", "/usr", "/bin", "/sbin")
    )
    if any(output_directory == root or root in output_directory.parents for root in protected_roots):
        raise RuntimeError(f"refusing a system build directory: {output_directory}")
    output_directory.mkdir(parents=True, exist_ok=True, mode=0o700)
    directory_info = output_directory.stat()
    if directory_info.st_uid != os.geteuid() or directory_info.st_mode & 0o022:
        raise RuntimeError(f"build directory must be user-owned and not group/world-writable: {output_directory}")

    output = output_directory / "vt_client"
    if output.is_symlink():
        raise RuntimeError(f"refusing a symlink worker path: {output}")
    if output.exists():
        output_info = output.lstat()
        if (not stat.S_ISREG(output_info.st_mode) or output_info.st_nlink != 1 or
                output_info.st_uid != os.geteuid() or output_info.st_mode & 0o022):
            raise RuntimeError(f"worker output must be a user-owned, non-writable regular file: {output}")

    backend = args.native_adapter.resolve(strict=True) if args.native_adapter else (
        HERE / "native_backend_unavailable.m"
    )
    frameworks = [
        "-framework", "Foundation",
        "-framework", "CoreMedia",
        "-framework", "CoreVideo",
        "-framework", "IOKit",
        "-framework", "VideoToolbox",
        "-framework", "Accelerate",
    ]
    with tempfile.TemporaryDirectory(prefix=".wex-hevc-worker-", dir=output_directory) as temporary:
        staged_worker = Path(temporary) / "vt_client"
        compiler = "clang++" if backend.suffix in {".cc", ".cpp", ".cxx", ".mm"} else "clang"
        run(
            "xcrun", compiler, "-arch", "x86_64", "-Wall", "-Wextra", "-Werror",
            "-fobjc-arc", "-O2", "-I", HERE, HERE / "worker.m", HERE / "pixel_input.m", backend,
            *frameworks, "-o", staged_worker,
        )
        run("codesign", "--force", "--sign", "-", staged_worker)
        run("codesign", "--verify", "--strict", staged_worker)
        run("lipo", staged_worker, "-verify_arch", "x86_64")
        run("python3", HERE / "probe_worker.py", staged_worker, "--scenario", "probe-" + args.expect)
        run("python3", HERE / "probe_worker.py", staged_worker, "--scenario", "open-invalid-registry")
        if args.expect == "unavailable":
            run("python3", HERE / "probe_worker.py", staged_worker, "--scenario", "open-no-backend")

        os.replace(staged_worker, output)
    print(f"Built {output}")


if __name__ == "__main__":
    main()
