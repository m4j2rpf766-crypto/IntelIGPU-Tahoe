#!/usr/bin/env python3
"""Exercise worker readiness and invalid-open protocol paths without frames."""

import argparse
import re
import socket
import struct
import subprocess
from pathlib import Path


MAGIC = 0x57484556
VERSION = 2
OPEN = 1
PROBE = 3
NO_BACKEND_STATUS = -12915
MESSAGE = struct.Struct("=IHHIi")
CONFIG = struct.Struct("@iiiiiQ")


def receive_exact(connection: socket.socket, size: int) -> bytes:
    chunks = bytearray()
    while len(chunks) < size:
        part = connection.recv(size - len(chunks))
        if not part:
            raise RuntimeError("worker closed the protocol socket early")
        chunks.extend(part)
    return bytes(chunks)


def current_accelerator_registry_id() -> int:
    result = subprocess.run(
        ["ioreg", "-p", "IOService", "-r", "-c", "IntelAccelerator", "-l", "-w0"],
        check=True,
        text=True,
        stdout=subprocess.PIPE,
    )
    match = re.search(
        r"IntelAccelerator\s+<class IntelAccelerator, id 0x([0-9a-fA-F]+)",
        result.stdout,
    )
    if not match:
        raise RuntimeError("no active IntelAccelerator registry entry was found")
    return int(match.group(1), 16)


def request_for_scenario(scenario: str) -> tuple[bytes, int, int, int]:
    if scenario.startswith("probe-"):
        expected_status = 0 if scenario == "probe-available" else NO_BACKEND_STATUS
        return MESSAGE.pack(MAGIC, VERSION, PROBE, 0, 0), expected_status, 0

    registry_id = 1 if scenario == "open-invalid-registry" else current_accelerator_registry_id()
    config = CONFIG.pack(1920, 1080, 5_500_000, 30_000, 1_001, registry_id)
    header = MESSAGE.pack(MAGIC, VERSION, OPEN, len(config), 0)
    expected_status = NO_BACKEND_STATUS
    expected_exit = 1
    return header + config, expected_status, expected_exit


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("worker", type=Path)
    parser.add_argument(
        "--scenario",
        choices=(
            "probe-unavailable",
            "probe-available",
            "open-no-backend",
            "open-invalid-registry",
        ),
        default="probe-unavailable",
    )
    args = parser.parse_args()
    worker = args.worker.resolve(strict=True)
    request, expected_status, expected_exit = request_for_scenario(args.scenario)

    parent, child = socket.socketpair()
    process = subprocess.Popen(
        [str(worker)],
        stdin=child.fileno(),
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        close_fds=True,
        pass_fds=(child.fileno(),),
    )
    child.close()

    try:
        parent.settimeout(10)
        parent.sendall(request)
        response = MESSAGE.unpack(receive_exact(parent, MESSAGE.size))
        magic, version, kind, response_bytes, status = response
        expected_kind = PROBE if args.scenario.startswith("probe-") else OPEN
        if (magic, version, kind) != (MAGIC, VERSION, expected_kind):
            raise RuntimeError(f"invalid worker reply header: {response!r}")
        if status != expected_status:
            raise RuntimeError(f"unexpected {args.scenario} status: {status}")

        if args.scenario == "probe-available":
            if response_bytes != 40:
                raise RuntimeError(f"invalid probe payload size: {response_bytes}")
            payload = receive_exact(parent, response_bytes)
            abi, size, vendor, device, registry, capabilities, width, height, reserved = (
                struct.unpack("=IIIIQIIII", payload)
            )
            if not (
                abi == 1
                and size == 40
                and vendor == 0x8086
                and device == 0x4680
                and registry != 0
                and (capabilities & 0xF) == 0xF
                and width >= 1920
                and height >= 1080
                and reserved == 0
            ):
                raise RuntimeError(f"invalid backend readiness payload: {payload!r}")
        elif response_bytes != 0:
            raise RuntimeError(f"unexpected response payload size: {response_bytes}")

        parent.shutdown(socket.SHUT_WR)
        parent.close()
        stderr = process.communicate(timeout=10)[1].decode("utf-8", "replace")
        if process.returncode != expected_exit:
            raise RuntimeError(
                f"worker exit for {args.scenario} was {process.returncode}, "
                f"expected {expected_exit}\n{stderr}"
            )
        print(f"PASS no-frame protocol: scenario={args.scenario} status={status}")
        if stderr:
            print(stderr, end="")
    finally:
        parent.close()
        if process.poll() is None:
            process.kill()
            process.wait()


if __name__ == "__main__":
    main()
