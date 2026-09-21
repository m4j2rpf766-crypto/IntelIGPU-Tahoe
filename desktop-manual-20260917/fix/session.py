#!/usr/bin/env python3
"""Single explicit session-only GPU startup. Never installed as a launch service."""
import argparse
import contextlib
import datetime
import fcntl
import hashlib
import json
import os
import pathlib
import plistlib
import subprocess
import time

from runtime_video import ensure_video, verify_bundle
from desktop_handoff import ensure_desktop

BASE = pathlib.Path(__file__).resolve().parent
ROOT = BASE.parents[1]
CONTROL = BASE / "manual-gate/control"
NATIVE = pathlib.Path("/Library/Extensions/ReimsTGLBoot.kext")
GATE = pathlib.Path("/Library/Extensions/ReimsADLManualActivation.kext")
LINK = pathlib.Path("/Library/Extensions/ReimsADLDesktopLink.kext")
METAL = pathlib.Path("/Library/GPUBundles/ReimsTahoeTGLGraphicsMTLDriver.bundle")
LINK_RECEIPT = ROOT / "desktop-reset-recovery-20260917/source/desktop-link/desktop-link-current.json"
LOCK = pathlib.Path("/private/var/run/reims-igpu-start.lock")


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def run(args, timeout=10):
    try:
        return subprocess.check_output(
            [str(value) for value in args], stderr=subprocess.STDOUT, timeout=timeout
        )
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired) as error:
        output = getattr(error, "output", None)
        if output:
            print(output.decode(errors="replace"), flush=True)
        raise


def registry():
    values = plistlib.loads(run(["ioreg", "-r", "-n", "GFX0", "-l", "-a"]))
    require(len(values) == 1, "Expected exactly one GFX0 registry root")
    return values[0]


def objects(root, class_name):
    result = []
    if root.get("IOObjectClass") == class_name:
        result.append(root)
    for child in root.get("IORegistryEntryChildren", []):
        result.extend(objects(child, class_name))
    return result


def action(name, timeout=10):
    print(run([CONTROL, name], timeout).decode(), flush=True)


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def verify_receipt_bundle(bundle, receipt):
    for relative, expected in receipt["hashes"].items():
        require(
            sha256(bundle / relative) == expected,
            f"Installed {receipt['identifier']} differs from this checkout build receipt: {relative}",
        )
    run(["codesign", "--verify", "--deep", "--strict", bundle], 30)


def loaded_entry(loaded, identifier, expected_uuid, required=True):
    lines = [line for line in loaded.splitlines() if identifier in line]
    if not lines:
        require(not required, f"Required kernel component is not loaded: {identifier}")
        return False
    require(len(lines) == 1, f"Expected one loaded {identifier}, found {len(lines)}")
    normalized = lines[0].upper().replace("-", "")
    require(
        expected_uuid.upper().replace("-", "") in normalized,
        f"Unexpected loaded version of {identifier}",
    )
    return True


def link_ready(pci):
    links = objects(pci, "ReimsADLDesktopLink")
    return (
        len(links) == 1
        and links[0].get("RCSADLPWorkaroundLive") is True
        and links[0].get("RCSResetPSMIRegistered") is True
    )


def hidden_ready(pci):
    accelerators = objects(pci, "IntelAccelerator")
    return (
        len(accelerators) == 1
        and not (accelerators[0].get("IOServiceState", 0) & 3)
        and pci.get("IOServiceBusyState") == 0
    )


def runtime_state(pci):
    accelerators = objects(pci, "IntelAccelerator")
    firmware = objects(pci, "IONDRVFramebuffer")
    native = objects(pci, "ReimsIntelADLFramebuffer")
    if not accelerators:
        require(len(firmware) == 1, "Cold startup requires exactly one firmware framebuffer")
        require(not native, "Native framebuffer exists without an IntelAccelerator")
        return "cold"
    require(len(accelerators) == 1, "Expected exactly one IntelAccelerator")
    state = accelerators[0].get("IOServiceState", 0)
    if state & 2:
        require(len(native) == 1, "Published accelerator lacks exactly one native framebuffer")
        require(link_ready(pci), "Published accelerator lacks a ready DesktopLink")
        return "published"
    require(hidden_ready(pci), "IntelAccelerator is neither hidden-ready nor published")
    require(
        len(firmware) == 1 and not native,
        "Prepared state must retain only the firmware framebuffer",
    )
    require(link_ready(pci), "Prepared accelerator lacks reset/display preparation")
    return "prepared"


def preflight():
    require(os.geteuid() == 0, "Run the single startup command as administrator")
    require(
        run(["sw_vers", "-buildVersion"]).strip() == b"25G83",
        "Unsupported macOS build",
    )

    runtime_receipt = json.loads((BASE / "deferred-runtime.json").read_text())
    gate_receipt = json.loads((BASE / "manual-gate-current.json").read_text())
    link_receipt = json.loads(LINK_RECEIPT.read_text())
    require(
        sha256(NATIVE / "Contents/MacOS/AppleIntelTGLGraphics")
        == runtime_receipt["candidate_sha256"],
        "Installed deferred TGL runtime differs from this checkout receipt",
    )
    run(["codesign", "--verify", "--deep", "--strict", NATIVE], 30)
    verify_receipt_bundle(GATE, gate_receipt)
    verify_receipt_bundle(LINK, link_receipt)
    run(["codesign", "--verify", "--deep", "--strict", METAL], 30)
    verify_bundle(run)

    loaded = run(["kmutil", "showloaded"]).decode()
    loaded_entry(
        loaded,
        "com.apple.driver.AppleIntelTGLGraphics",
        runtime_receipt["candidate_uuid"],
    )
    loaded_entry(
        loaded,
        gate_receipt["identifier"],
        gate_receipt["uuid"],
    )
    pci = registry()
    require(
        pci.get("device-id") == bytes.fromhex("ffff0000"),
        "FFFF startup isolation changed",
    )

    link_loaded = loaded_entry(
        loaded,
        link_receipt["identifier"],
        link_receipt["uuid"],
        required=False,
    )
    if not link_loaded:
        require(
            runtime_state(pci) == "cold",
            "DesktopLink is absent after GPU initialization; preserve evidence",
        )
        print(
            run(["kmutil", "load", "-p", LINK, "--load-style", "start-only"], 30).decode(),
            flush=True,
        )
        loaded = run(["kmutil", "showloaded"]).decode()
        loaded_entry(loaded, link_receipt["identifier"], link_receipt["uuid"])
    return registry()


def prepare_runtime(pci):
    require(runtime_state(pci) == "cold", "Prepare requested outside cold startup state")
    if not objects(pci, "ReimsADLManualMatchProbe"):
        action("SelfTest")
    deadline = time.monotonic() + 5
    while not objects(registry(), "ReimsADLManualMatchProbe"):
        require(
            time.monotonic() < deadline,
            "Read-only matching self-test did not appear",
        )
        time.sleep(0.25)

    action("PrepareRuntime", 50)
    deadline = time.monotonic() + 45
    while True:
        pci = registry()
        accelerators = objects(pci, "IntelAccelerator")
        require(
            not any(value.get("IOServiceState", 0) & 2 for value in accelerators),
            "Premature accelerator publication; stop and preserve evidence",
        )
        if hidden_ready(pci):
            break
        require(
            time.monotonic() < deadline,
            "Hidden initialization did not finish; display commit was not attempted",
        )
        time.sleep(1)

    require(
        objects(pci, "IONDRVFramebuffer"),
        "Firmware display disappeared before commit",
    )
    action("PrepareDisplay")
    pci = registry()
    require(
        runtime_state(pci) == "prepared",
        "Display/reset preparation did not reach the prepared state",
    )
    print(
        "Prepared safely; firmware framebuffer remained active until the verified commit boundary.",
        flush=True,
    )
    return pci


def commit_display(pci):
    require(
        runtime_state(pci) == "prepared",
        "Hidden preparation is required before display commit",
    )
    action("CommitDisplay", 15)
    deadline = time.monotonic() + 10
    while True:
        pci = registry()
        accelerators = objects(pci, "IntelAccelerator")
        if len(accelerators) == 1 and accelerators[0].get("IOServiceState", 0) & 2:
            break
        require(
            time.monotonic() < deadline,
            "Commit returned but accelerator publication was not observed",
        )
        time.sleep(0.5)
    require(
        runtime_state(pci) == "published",
        "Display publication did not reach the supported state",
    )
    print(
        "Display route published; continuing with the required video runtime.",
        flush=True,
    )
    return pci


def activate(pci, logdir):
    processed = []
    state = runtime_state(pci)
    print(f"Detected runtime state: {state}", flush=True)
    if state == "cold":
        pci = prepare_runtime(pci)
        processed.append("prepare")
        state = runtime_state(pci)
    if state == "prepared":
        pci = commit_display(pci)
        processed.append("commit")
    require(runtime_state(pci) == "published", "Display runtime is not published")
    try:
        pci = ensure_video(run, registry, objects, logdir)
    except Exception:
        try:
            (logdir / "video-failed.plist").write_bytes(plistlib.dumps(registry()))
        except Exception:
            pass
        print(
            "Display activation is not repeated or rolled back. Resolve the recorded video approval/error, then rerun only igpu-start.",
            flush=True,
        )
        raise
    processed.append("video-verify")
    return pci, processed


@contextlib.contextmanager
def exclusive_start():
    descriptor = os.open(LOCK, os.O_RDWR | os.O_CREAT, 0o600)
    try:
        try:
            fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise RuntimeError("Another igpu-start process is already running") from error
        yield
    finally:
        os.close(descriptor)


def save_failure(logdir, stage, error):
    failure = {
        "failed_at": datetime.datetime.now().astimezone().isoformat(),
        "stage": stage,
        "error_type": type(error).__name__,
        "error": str(error),
    }
    (logdir / "failure.json").write_text(json.dumps(failure, indent=2) + "\n")
    try:
        (logdir / "failed.plist").write_bytes(plistlib.dumps(registry()))
    except Exception:
        pass


def main():
    parser = argparse.ArgumentParser(
        description="Start or finish the approved Intel iGPU runtime using one state-aware command."
    )
    parser.parse_args()
    require(os.geteuid() == 0, "Run the single startup command as administrator")
    with exclusive_start():
        logdir = BASE / (
            "session-"
            + datetime.datetime.now().strftime("%Y%m%d-%H%M%S-%f")
            + "-start"
        )
        logdir.mkdir()
        print(f'Startup evidence: {logdir}', flush=True)
        print('If desktop handoff is needed, this will end the current desktop session. Save work first.', flush=True)
        child = os.fork()
        if child:
            _, status = os.waitpid(child, 0)
            os._exit(os.WEXITSTATUS(status) if os.WIFEXITED(status) else 1)
        os.setsid()
        with open(os.devnull, 'rb') as source, (logdir / 'startup.log').open('a', buffering=1) as output:
            os.dup2(source.fileno(), 0)
            os.dup2(output.fileno(), 1)
            os.dup2(output.fileno(), 2)
        try:
            pci = preflight()
        except Exception as error:
            save_failure(logdir, "preflight", error)
            print(
                f"Preflight stopped before display mutation. Evidence: {logdir}",
                flush=True,
            )
            raise
        (logdir / "before.plist").write_bytes(plistlib.dumps(pci))
        initial_state = runtime_state(pci)
        try:
            pci, processed = activate(pci, logdir)
            desktop = ensure_desktop(run, registry, objects, logdir)
            processed.append('desktop-handoff-verified')
            pci = registry()
        except Exception as error:
            save_failure(logdir, "activation", error)
            print(f"Startup stopped safely. Evidence: {logdir}", flush=True)
            raise
        (logdir / "after.plist").write_bytes(plistlib.dumps(pci))
        result = {
            "completed_at": datetime.datetime.now().astimezone().isoformat(),
            "initial_state": initial_state,
            "final_state": runtime_state(pci),
            "phases_processed": processed,
            "video_runtime_verified": True,
            "windowserver_and_flip_acceptance_required": False,
            "desktop": desktop,
            "visible_desktop_user_confirmation_required": True,
        }
        (logdir / "result.json").write_text(json.dumps(result, indent=2) + "\n")
        print("Kernel display, video runtime and completed desktop flips verified.", flush=True)
        print(
            "Visible desktop still requires user confirmation.",
            flush=True,
        )
        print("Evidence:", logdir, flush=True)


if __name__ == "__main__":
    main()
