"""Verify the private backup and OTA package; print safe USB commands.

Read-only physical flash probe; no device writes. Single-slot layouts need explicit
--install-ota-layout; NVS contents, address AND size are retained.
"""
import argparse
import binascii
import hashlib
import json
import shlex
import struct
from pathlib import Path


def partitions(data):
    if len(data) != 4096:
        raise ValueError("Complete partition sector required")
    rows = []
    checked = False
    for position in range(0, 3072, 32):
        row = data[position:position + 32]
        if row[:2] == b"\xeb\xeb":
            if row[2:16] != b"\xff" * 14 or row[16:] != hashlib.md5(data[:position], usedforsecurity=False).digest():
                raise ValueError("Partition checksum mismatch")
            checked = True
            break
        if row[:2] != b"\xaa\x50":
            raise ValueError("Invalid partition table")
        _, kind, subtype, offset, size, name, flags = struct.unpack("<2sBBLL16sL", row)
        if flags or not size or offset < 0x9000 or offset + size > 0x1000000:
            raise ValueError("Unsupported partition bounds/encryption")
        rows.append((kind, subtype, offset, size, name.rstrip(b"\x00"), flags))
    if not checked:
        raise ValueError("Partition checksum missing")
    intervals = sorted((p[2], p[2] + p[3]) for p in rows)
    if any(a[1] > b[0] for a, b in zip(intervals, intervals[1:])):
        raise ValueError("Partition overlap")
    return rows


def encode_table(rows):
    raw = b"".join(struct.pack("<2sBBLL16sL", b"\xaa\x50", *row) for row in rows)
    raw += b"\xeb\xeb" + b"\xff" * 14 + hashlib.md5(raw, usedforsecurity=False).digest()
    return raw.ljust(3072, b"\xff")


def active_slot(otadata):
    if len(otadata) != 8192:
        raise ValueError("Complete OTA data required")
    records = []
    for offset in (0, 4096):
        row = otadata[offset:offset + 32]
        if row == b"\xff" * 32:
            continue
        sequence, state, crc = struct.unpack_from("<I20xII", row)
        expected = binascii.crc32(struct.pack("<I", sequence), 0xFFFFFFFF) & 0xFFFFFFFF
        if sequence < 0xFFFFFFFF and crc == expected:
            if state == 1:
                raise ValueError("Pending trial boot: let the sensor finish validation first")
            if state not in (3, 4):
                records.append(sequence)
        else:
            raise ValueError("Ambiguous or corrupt boot selection")
    if not records:
        if otadata[:32] == b"\xff" * 32 and otadata[4096:4128] == b"\xff" * 32:
            return 0x20000
        raise ValueError("No valid boot selection")
    if max(records) == 0:
        raise ValueError("No positive boot sequence")
    return 0x20000 if (max(records) - 1) % 2 == 0 else 0x320000


def usb_plan(backup, manifest, directory, migrate=False):
    if len(backup) != 0x1000000:
        raise ValueError("Complete private 16 MiB backup required")
    rows = partitions(backup[0x8000:0x9000])
    nvs = [p for p in rows if p[0:2] == (1, 2) and p[4] == b"nvs"]
    if len(nvs) != 1 or nvs[0][2] != 0x9000 or not 0x4000 <= nvs[0][3] <= 0x6000:
        raise ValueError("Unsupported NVS layout; review separately")
    apps = [p for p in rows if p[0] == 0]
    slots = {(p[1], p[2], p[3], p[4]) for p in apps}
    standard = {(0x10, 0x20000, 0x300000, b"app0"), (0x11, 0x320000, 0x300000, b"app1")}
    metadata = [p for p in rows if p[0:2] == (1, 0)]
    files = [(0, directory / "bootloader.bin")]
    if slots == standard and len(metadata) == 1 and metadata[0][2:4] == (0xf000, 0x2000):
        offset = active_slot(backup[0xf000:0x11000])
    elif len(apps) == 1 and apps[0][1] in (0, 0x10) and apps[0][2] == 0x10000:
        if not migrate:
            raise ValueError("One-slot Direct layout: explicit --install-ota-layout required")
        # Preserve NVS exactly. Other flash contents stay in the private backup.
        table_rows = [nvs[0], (1, 0, 0xf000, 0x2000, b"otadata", 0),
                      (0, 0x10, 0x20000, 0x300000, b"app0", 0),
                      (0, 0x11, 0x320000, 0x300000, b"app1", 0),
                      (1, 0x82, 0x620000, 0x9d0000, b"spiffs", 0),
                      (1, 3, 0xff0000, 0x10000, b"coredump", 0)]
        replacement = directory / "partitions-retain-nvs.bin"
        replacement.write_bytes(encode_table(table_rows))
        files += [(0x8000, replacement), (0xf000, directory / "boot_app0.bin")]
        offset = 0x20000
    else:
        raise ValueError("Unknown flash layout; no safe generic migration")
    if manifest["files"]["firmware.bin"]["bytes"] > 0x300000:
        raise ValueError("Application does not fit")
    files.append((offset, directory / "firmware.bin"))
    if any(address < 0x9000 < address + path.stat().st_size for address, path in files):
        raise ValueError("NVS overlap")
    return files


def verify_package(directory):
    manifest = json.loads((directory / "manifest.json").read_text())
    if (manifest.get("environment") != "direct_biolingo_v22_psram_ota_production" or
            manifest.get("ota_profile") != "biolingo-v22-n16r8-direct-ota-v1" or
            manifest.get("sample_rate") != 380 or manifest.get("endpoint") != "https://green-mind.ch"):
        raise ValueError("Wrong firmware variant")
    expected = {"firmware.bin", "bootloader.bin", "partitions.bin", "boot_app0.bin"}
    if set(manifest["files"]) != expected:
        raise ValueError("Unexpected flash files")
    for name, info in manifest["files"].items():
        data = (directory / name).read_bytes()
        if len(data) != info["bytes"] or hashlib.sha256(data).hexdigest() != info["sha256"]:
            raise ValueError("Package checksum mismatch: " + name)
    image = (directory / "firmware.bin").read_bytes()
    if image[0] != 0xE9 or image[12:14] != b"\x09\x00":
        raise ValueError("Wrong ESP image")
    return manifest


def main():
    from flash_hardware import probe_flash, require_capacity
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--backup", type=Path, required=True)
    parser.add_argument("--port", required=True)
    parser.add_argument("--install-ota-layout", action="store_true")
    args = parser.parse_args()
    directory = Path(__file__).resolve().parent
    manifest = verify_package(directory)
    detected = probe_flash(args.port)
    require_capacity(detected, manifest["flash_bytes"], args.backup.stat().st_size)
    files = usb_plan(args.backup.read_bytes(), manifest, directory, args.install_ota_layout)
    print("Verified OTA bootstrap:", manifest["version"])
    print("N16R8 / 16 MiB / 8 MiB PSRAM must be physically verified first.")
    print("NVS pairing is retained. Original flash backup stays private.")
    print("No device write occurred; review these commands for the connected sensor.")
    base = ["python", "-m", "esptool", "--chip", "esp32s3", "--port", args.port, "--baud", "460800"]
    arguments = [value for offset, path in files for value in (hex(offset), str(path))]
    print(shlex.join(base + ["--after", "no_reset", "write_flash"] + arguments))
    print(shlex.join(base + ["--after", "hard_reset", "verify_flash"] + arguments))


if __name__ == "__main__":
    main()
