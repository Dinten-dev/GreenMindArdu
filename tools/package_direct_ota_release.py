"""Publishable OTA bootstrap uses the exact physically tested application.

CI recompilation is a software candidate; it cannot substitute this binary.
No private NVS or whole-flash images are included.
"""
import argparse
import hashlib
import json
import re
from pathlib import Path
from zipfile import ZIP_DEFLATED, ZipFile

ROOT = Path(__file__).resolve().parents[1]
ENV = "direct_biolingo_v22_psram_ota_production"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-commit", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--hardware-report", type=Path, required=True)
    args = parser.parse_args()
    if not re.fullmatch(r"[0-9a-f]{40}", args.source_commit):
        raise ValueError("Exact source commit required")
    proof = json.loads(args.hardware_report.read_text())
    required = {"usb_bootstrap", "ota_forward", "ota_return", "boot_confirmed", "rollback_test", "bad_hash_rejected"}
    if not all(proof.get("results", {}).get(test) is True for test in required):
        raise ValueError("Complete physical OTA acceptance required")
    for name, digest in proof["source_files_sha256"].items():
        if hashlib.sha256((ROOT / name).read_bytes()).hexdigest() != digest:
            raise ValueError("Source differs from physically tested source")
    config = (ROOT / "examples/direct_sensor/src/CloudConfig.h").read_text()
    version = re.search(r'#define GREENMIND_DIRECT_OTA_VERSION "([^"]+)"', config).group(1)
    build = ROOT / "examples/direct_sensor/.pio/build" / ENV
    files = {name: (build / name).read_bytes() for name in ("firmware.bin", "bootloader.bin", "partitions.bin")}
    # Standard two-slot selector; contains no sensor provisioning.
    files["boot_app0.bin"] = (Path.home() / ".platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin").read_bytes()
    if (hashlib.sha256(files["firmware.bin"]).hexdigest() != proof["firmware_sha256"] or
            hashlib.sha256(files["bootloader.bin"]).hexdigest() != proof["bootloader_sha256"] or
            version != proof["version"] or not 1024 <= len(files["firmware.bin"]) <= 0x300000):
        raise ValueError("Binary differs from physically tested application/bootloader")
    manifest = {"source_commit": args.source_commit, "environment": ENV, "version": version,
        "board": "Biolingo v22 ESP32-S3-WROOM-1-N16R8", "flash_bytes": 0x1000000,
        "psram_required": True, "ota_profile": "biolingo-v22-n16r8-direct-ota-v1",
        "endpoint": "https://green-mind.ch", "sample_rate": 380, "sample_bits": 16, "channels": 1,
        "calibration_version": "unsigned-mv-linear-int16-v1", "sampling_filter": "avg20ms", "adc_sample_rate": 2000, "filter_samples": 40, "filter_group_delay_us": 9750, "adc_clipping_recoverable": False,
        "distribution": "one-time-usb-bootstrap-then-device-scoped-cloud-ota",
        "spool": "volatile PSRAM, up to 4 MiB; no reset/power-loss persistence",
        "update_gap": "deliberate maintenance pause/reboot, after acknowledged buffer drain",
        "hardware_validation": "one N16R8: USB bootstrap, cloud OTA both slots, bad hash, trial rollback",
        "files": {name: {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()} for name, data in files.items()}}
    files["manifest.json"] = (json.dumps(manifest, indent=2) + "\n").encode()
    files["hardware-test.json"] = (json.dumps(proof, indent=2) + "\n").encode()
    files["README.md"] = (ROOT / "docs/direct-upgrade.md").read_bytes()
    files["flash_hardware.py"] = (ROOT / "tools/flash_hardware.py").read_bytes()
    files["prepare_direct_ota_upgrade.py"] = (ROOT / "tools/prepare_direct_ota_upgrade.py").read_bytes()
    args.output.mkdir(parents=True, exist_ok=True)
    target = args.output / "direct-to-cloud-production.zip"
    with ZipFile(target, "w", ZIP_DEFLATED) as package:
        for name, data in files.items():
            package.writestr(name, data)
    with ZipFile(target) as package:
        if set(package.namelist()) != set(files) or package.testzip():
            raise ValueError("Package integrity failed")
    for name in ("manifest.json", "hardware-test.json"):
        (args.output / name).write_bytes(files[name])
    (args.output / "firmware.bin").write_bytes(files["firmware.bin"])
    (args.output / "SHA256SUMS").write_text(
        hashlib.sha256(target.read_bytes()).hexdigest() + "  " + target.name + "\n" +
        hashlib.sha256(files["firmware.bin"]).hexdigest() + "  firmware.bin\n")
    print(json.dumps({"package": str(target), "firmware_sha256": manifest["files"]["firmware.bin"]["sha256"],
                      "physical_acceptance": True, "source_commit": args.source_commit}))


if __name__ == "__main__":
    main()
