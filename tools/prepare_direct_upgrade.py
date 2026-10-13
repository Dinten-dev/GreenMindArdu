"""Verify a local backup and package; print application-only USB commands.

Never opens a serial device, writes flash, or uploads the private backup.
Partition format follows Espressif gen_esp32part.py (Arduino framework).
"""

import argparse
import hashlib
import json
import shlex
import struct
from pathlib import Path


def application_partition(table, image_size, active_offset=None):
    if len(table) != 4096:
        raise ValueError("Partition table must be a complete 4096-byte sector")
    entries, checked = [], False
    for position in range(0, 3072, 32):
        item = table[position:position + 32]
        if item[:2] == b"\xeb\xeb":
            digest = hashlib.md5(table[:position], usedforsecurity=False).digest()
            if item[2:16] != b"\xff" * 14 or item[16:] != digest:
                raise ValueError("Partition checksum mismatch")
            checked = True
            break
        if item[:2] != b"\xaa\x50":
            raise ValueError("Invalid or unchecksummed partition table")
        _, kind, subtype, offset, size, name, flags = struct.unpack("<2sBBLL16sL", item)
        if flags or offset < 0x9000 or not size or offset + size > 0x1000000:
            raise ValueError("Unsupported encrypted, flagged or out-of-range partition")
        entries.append((kind, subtype, offset, size, name))
    if not checked:
        raise ValueError("Partition checksum missing")
    intervals = sorted((e[2], e[2] + e[3]) for e in entries)
    if any(a[1] > b[0] for a, b in zip(intervals, intervals[1:])):
        raise ValueError("Overlapping partitions")
    apps = [e for e in entries if e[0] == 0 and e[1] in (0, 0x10, 0x11)]
    if active_offset is None:
        if len(apps) != 1:
            raise ValueError("Multiple applications: supply the known active --app-offset; do not guess")
        selected = apps[0]
    else:
        matching = [e for e in apps if e[2] == active_offset]
        if len(matching) != 1:
            raise ValueError("Offset is not an application in this sensor's partition table")
        selected = matching[0]
    if selected[2] % 0x10000 or image_size <= 0 or image_size > selected[3]:
        raise ValueError("Firmware does not fit the application partition")
    return selected[2]


def verify_package(directory):
    manifest = json.loads((directory / "manifest.json").read_text())
    if (manifest["environment"] != "direct_biolingo_v22_psram_production"
            or manifest["distribution"] != "manual-usb-application-only"
            or manifest["sample_rate"] != 380 or manifest["endpoint"] != "https://green-mind.ch"):
        raise ValueError("Wrong firmware variant")
    item = manifest["firmware"]
    if item["file"] != "firmware.bin":
        raise ValueError("Unexpected firmware filename")
    image = directory / "firmware.bin"
    data = image.read_bytes()
    if len(data) != item["bytes"] or hashlib.sha256(data).hexdigest() != item["sha256"]:
        raise ValueError("Firmware checksum mismatch")
    return image, len(data), manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--backup", required=True, type=Path)
    parser.add_argument("--port", required=True)
    parser.add_argument("--app-offset", type=lambda value: int(value, 0))
    args = parser.parse_args()
    image, size, manifest = verify_package(Path(__file__).resolve().parent)
    if args.backup.stat().st_size != 0x1000000:
        raise ValueError("A complete private 16-MiB flash backup is required")
    with args.backup.open("rb") as backup:
        backup.seek(0x8000)
        table = backup.read(4096)
    offset = application_partition(table, size, args.app_offset)
    print("Verified package:", manifest["version"])
    print("Application only; pairing, bootloader, partition table and boot selection stay intact.")
    print("No device write has occurred. Confirm this backup belongs to the connected sensor.")
    command = ["python", "-m", "esptool", "--chip", "esp32s3", "--port", args.port,
               "--baud", "460800"]
    print(shlex.join(command + ["--after", "no_reset", "write_flash", hex(offset), str(image)]))
    print(shlex.join(command + ["--after", "hard_reset", "verify_flash", hex(offset), str(image)]))


if __name__ == "__main__":
    main()
