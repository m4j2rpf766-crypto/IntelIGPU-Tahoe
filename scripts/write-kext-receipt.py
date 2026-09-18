#!/usr/bin/env python3
"""Write the exact identity that session.py must accept for a local kext build."""
import argparse
import hashlib
import json
import pathlib
import plistlib
import re
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument("bundle", type=pathlib.Path)
parser.add_argument("executable")
parser.add_argument("output", type=pathlib.Path)
args = parser.parse_args()

bundle = args.bundle.resolve()
binary = bundle / "Contents/MacOS" / args.executable
info_path = bundle / "Contents/Info.plist"
info = plistlib.loads(info_path.read_bytes())
uuid_output = subprocess.check_output(["dwarfdump", "--uuid", str(binary)], text=True)
match = re.search(r"UUID: ([0-9A-F-]{36}) \(x86_64\)", uuid_output)
assert match, uuid_output

receipt = {
    "bundle": bundle.name,
    "identifier": info["CFBundleIdentifier"],
    "version": info["CFBundleVersion"],
    "uuid": match.group(1),
    "hashes": {
        "Contents/MacOS/" + args.executable: hashlib.sha256(binary.read_bytes()).hexdigest(),
        "Contents/Info.plist": hashlib.sha256(info_path.read_bytes()).hexdigest(),
    },
}
args.output.write_text(json.dumps(receipt, indent=2) + "\n")
print(json.dumps(receipt, indent=2))
