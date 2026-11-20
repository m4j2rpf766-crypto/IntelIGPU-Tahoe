#!/usr/bin/env python3
"""Build a workspace-only WEX Intel HEVC Metal driver bundle.

The output uses an ad-hoc signature by default and is diagnostic only. It is
never installed by this script. System use requires a signature accepted by
the target macOS configuration and the corresponding driver-owned selector.
"""

from __future__ import annotations

import argparse
import os
import plistlib
import re
import shutil
import stat
import subprocess
import tempfile
from pathlib import Path


HERE = Path(__file__).resolve().parent
NATIVE_BUNDLE = Path(
    "/Library/GPUBundles/AppleIntelTGLGraphicsMTLDriver.bundle"
)
NATIVE_IDENTIFIER = "com.apple.driver.AppleIntelTGLGraphicsMTLDriver"
NATIVE_EXECUTABLE = "AppleIntelTGLGraphicsMTLDriver"
NATIVE_PRINCIPAL = "MTLIGAccelDevice"

BUNDLE_NAME = "WEXHEVCMetalDriver.bundle"
EXECUTABLE_NAME = "WEXHEVCMetalDriver"
BUNDLE_IDENTIFIER = "org.whateverxe.driver.WEXHEVCMetalDriver"
PRINCIPAL_CLASS = "WEXIntelTGLMetalDevice"
WORKER_RELATIVE_PATH = Path(
    "Contents/MacOS/WEXHEVCBackend/Contents/MacOS/vt_client"
)
BUILD_MARKER = "org.whateverxe.driver.WEXHEVCMetalDriver workspace build"


def run(
    *args: object,
    capture: bool = False,
    timeout: float = 120,
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(arg) for arg in args],
        check=True,
        timeout=timeout,
        text=True,
        stdout=subprocess.PIPE if capture else None,
        stderr=subprocess.PIPE if capture else None,
    )


def plist(path: Path) -> dict[str, object]:
    with path.open("rb") as source:
        value = plistlib.load(source)
    if not isinstance(value, dict):
        raise RuntimeError(f"expected a plist dictionary: {path}")
    return value


def validate_native_driver() -> str:
    if not NATIVE_BUNDLE.is_dir() or NATIVE_BUNDLE.is_symlink():
        raise RuntimeError(f"required installed native TGL bundle is missing: {NATIVE_BUNDLE}")
    info = plist(NATIVE_BUNDLE / "Contents/Info.plist")
    if (
        info.get("CFBundleIdentifier") != NATIVE_IDENTIFIER
        or info.get("CFBundleExecutable") != NATIVE_EXECUTABLE
        or info.get("NSPrincipalClass") != NATIVE_PRINCIPAL
    ):
        raise RuntimeError("the installed bundle is not the expected TGL Metal driver")

    executable = NATIVE_BUNDLE / "Contents/MacOS" / NATIVE_EXECUTABLE
    run("lipo", executable, "-verify_arch", "x86_64")
    run("codesign", "--verify", "--strict", NATIVE_BUNDLE)
    detail = run("codesign", "-dv", "--verbose=4", NATIVE_BUNDLE, capture=True)
    signature_text = f"{detail.stdout}\n{detail.stderr}"
    match = re.search(r"^CDHash=([A-Fa-f0-9]{40})$", signature_text, re.MULTILINE)
    if not match:
        raise RuntimeError("could not read the installed native TGL driver CDHash")
    return match.group(1).lower()


def validate_worker(worker: Path) -> Path:
    if worker.is_symlink():
        raise RuntimeError("worker must not be a symbolic link")
    worker = worker.resolve(strict=True)
    if not worker.is_file():
        raise RuntimeError("worker must be a regular, non-symlink executable")
    if not os.access(worker, os.X_OK):
        raise RuntimeError(f"worker is not executable: {worker}")
    run("lipo", worker, "-verify_arch", "x86_64")
    run("codesign", "--verify", "--strict", worker)
    return worker


def create_probe_source(path: Path) -> None:
    path.write_text(
        r'''#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <stdio.h>
#include <string.h>

int main(int argc, const char **argv) {
    @autoreleasepool {
        if (argc != 2) return 2;
        NSString *path = [NSString stringWithUTF8String:argv[1]];
        NSBundle *bundle = [NSBundle bundleWithPath:path];
        NSError *error = nil;
        if (!bundle || ![bundle loadAndReturnError:&error]) {
            fprintf(stderr, "bundle load failed: %s\n",
                    error.localizedDescription.UTF8String ?: "unknown error");
            return 3;
        }

        Class principal = bundle.principalClass;
        Class native = NSClassFromString(@"MTLIGAccelDevice");
        SEL initializer = sel_registerName("initWithAcceleratorPort:");
        SEL graphicsAnchor = sel_registerName("newBufferWithLength:options:");
        Method wrappedInit = principal ? class_getInstanceMethod(principal, initializer) : NULL;
        Method nativeInit = native ? class_getInstanceMethod(native, initializer) : NULL;
        Method wrappedGraphics = principal ? class_getInstanceMethod(principal, graphicsAnchor) : NULL;
        Method nativeGraphics = native ? class_getInstanceMethod(native, graphicsAnchor) : NULL;

        if (!principal || !native || class_getSuperclass(principal) != native ||
            !wrappedInit || !nativeInit ||
            strcmp(method_getTypeEncoding(wrappedInit), "@20@0:8I16") != 0 ||
            method_getImplementation(wrappedInit) == method_getImplementation(nativeInit) ||
            !wrappedGraphics || !nativeGraphics ||
            method_getImplementation(wrappedGraphics) != method_getImplementation(nativeGraphics)) {
            fputs("principal class did not preserve the native TGL Metal ABI\n", stderr);
            return 4;
        }

        printf("PASS no-GPU Metal bundle exercise: principal=%s superclass=%s; "
               "initWithAcceleratorPort: override ABI=%s; graphics anchor inherited\n",
               class_getName(principal), class_getName(native),
               method_getTypeEncoding(wrappedInit));
        return 0;
    }
}
''',
        encoding="utf-8",
    )


def validate_previous_bundle(bundle: Path, out: Path) -> None:
    if bundle.parent != out or bundle.name != BUNDLE_NAME or bundle.is_symlink():
        raise RuntimeError("refusing to replace a bundle outside the selected workspace output")
    if not bundle.exists():
        return
    if not bundle.is_dir():
        raise RuntimeError(f"refusing to replace a non-directory artifact: {bundle}")

    info = plist(bundle / "Contents/Info.plist")
    if (
        info.get("CFBundleIdentifier") != BUNDLE_IDENTIFIER
        or info.get("CFBundleExecutable") != EXECUTABLE_NAME
        or info.get("WEXWorkspaceBuildMarker") != BUILD_MARKER
    ):
        raise RuntimeError(f"refusing to replace an unrecognized bundle: {bundle}")


def replace_previous_bundle(staged: Path, bundle: Path, out: Path) -> None:
    validate_previous_bundle(bundle, out)
    if not bundle.exists():
        staged.rename(bundle)
        return

    previous = out / f".{BUNDLE_NAME}.previous"
    if previous.exists() or previous.is_symlink():
        raise RuntimeError(f"refusing to replace an existing rollback path: {previous}")
    bundle.rename(previous)
    try:
        staged.rename(bundle)
    except OSError:
        previous.rename(bundle)
        raise
    shutil.rmtree(previous)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--worker",
        type=Path,
        required=True,
        help="signed x86_64 vt_client worker built for this OS and Intel backend",
    )
    parser.add_argument(
        "--out",
        type=Path,
        default=Path(tempfile.gettempdir()) / "WEXHEVCMetalDriver-build",
        help="user-owned build output directory outside File Provider folders",
    )
    parser.add_argument(
        "--signing-identity",
        default="-",
        help="codesign identity; '-' produces a local diagnostic-only ad-hoc signature",
    )
    args = parser.parse_args()

    worker = validate_worker(args.worker)
    native_hash = validate_native_driver()
    out_argument = args.out.expanduser()
    if out_argument.is_symlink():
        raise RuntimeError("output directory must not be a symbolic link")
    if out_argument.exists() and not out_argument.is_dir():
        raise RuntimeError(f"output path is not a directory: {out_argument}")
    out_argument.mkdir(parents=True, exist_ok=True)
    out = out_argument.resolve(strict=True)
    output_info = out.stat()
    if output_info.st_uid != os.geteuid():
        raise RuntimeError("output directory must be owned by the current user")
    if output_info.st_mode & (stat.S_IWGRP | stat.S_IWOTH):
        raise RuntimeError("output directory must not be writable by other users")
    bundle = out / BUNDLE_NAME
    validate_previous_bundle(bundle, out)

    with tempfile.TemporaryDirectory(prefix=".wex-hevc-stage-", dir=out) as stage_dir:
        staged = Path(stage_dir) / BUNDLE_NAME
        contents = staged / "Contents"
        executable_dir = contents / "MacOS"
        helper = staged / WORKER_RELATIVE_PATH
        executable_dir.mkdir(parents=True)
        helper.parent.mkdir(parents=True)
        shutil.copy2(worker, helper)
        run("codesign", "--verify", "--strict", helper)

        info = {
            "CFBundleDevelopmentRegion": "English",
            "CFBundleExecutable": EXECUTABLE_NAME,
            "CFBundleIdentifier": BUNDLE_IDENTIFIER,
            "CFBundleInfoDictionaryVersion": "6.0",
            "CFBundleName": "WhateverXe Intel HEVC Metal Driver",
            "CFBundlePackageType": "BNDL",
            "CFBundleShortVersionString": "0.1.0",
            "CFBundleVersion": "1",
            "NSPrincipalClass": PRINCIPAL_CLASS,
            "WEXNativeMetalDriverBundleIdentifier": NATIVE_IDENTIFIER,
            "WEXNativeMetalDriverCDHash": native_hash,
            "WEXNativeMetalDriverExecutable": NATIVE_EXECUTABLE,
            "WEXNativeMetalDriverPath": str(NATIVE_BUNDLE),
            "WEXNativeMetalDriverPrincipalClass": NATIVE_PRINCIPAL,
            "WEXSelectedAccelerator": "8086:4680",
            "WEXSelectedMetalPluginName": BUNDLE_NAME.removesuffix(".bundle"),
            "WEXWorkspaceBuildMarker": BUILD_MARKER,
        }
        with (contents / "Info.plist").open("wb") as destination:
            plistlib.dump(info, destination, sort_keys=True)

        source_files = (
            HERE / "metal_entry.m",
            HERE / "pixel_input.m",
            HERE / "provider.m",
            HERE / "transport.m",
        )
        for source in source_files:
            if not source.is_file():
                raise RuntimeError(f"required source is missing: {source.name}")

        executable = executable_dir / EXECUTABLE_NAME
        frameworks = [
            "-framework", "Accelerate",
            "-framework", "Foundation",
            "-framework", "VideoToolbox",
            "-framework", "CoreMedia",
            "-framework", "CoreVideo",
            "-framework", "CoreFoundation",
            "-framework", "IOKit",
            "-framework", "IOSurface",
            "-framework", "Security",
        ]
        run(
            "xcrun", "clang", "-arch", "x86_64", "-Wall", "-Wextra", "-Werror",
            "-fobjc-arc", "-bundle", "-Wl,-undefined,error",
            *source_files, *frameworks, "-o", executable,
        )

        run("codesign", "--force", "--sign", args.signing_identity, staged)
        run("codesign", "--verify", "--deep", "--strict", staged)
        run("codesign", "--verify", "--strict", helper)
        run("lipo", executable, "-verify_arch", "x86_64")

        probe_result: subprocess.CompletedProcess[str]
        with tempfile.TemporaryDirectory(prefix="wex-metal-probe-") as temporary:
            temp = Path(temporary)
            source = temp / "probe.m"
            probe = temp / "probe"
            create_probe_source(source)
            run(
                "xcrun", "clang", "-arch", "x86_64", "-Wall", "-Wextra", "-Werror",
                "-fobjc-arc", source, "-framework", "Foundation", "-o", probe,
            )
            run("codesign", "--force", "--sign", args.signing_identity, probe)
            probe_result = run(probe, staged, capture=True, timeout=20)
            print(probe_result.stdout, end="")

        replace_previous_bundle(staged, bundle, out)

    run("codesign", "--verify", "--deep", "--strict", bundle)
    run("codesign", "--verify", "--strict", bundle / WORKER_RELATIVE_PATH)

    if args.signing_identity == "-":
        print("Signature: ad-hoc diagnostic only; system use needs a trusted signing identity.")
    print(bundle)


if __name__ == "__main__":
    main()
