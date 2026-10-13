"""Package only the Production N16R8 Direct application; no NVS or flash dump."""

import argparse
import hashlib
import json
import re
from pathlib import Path
from zipfile import ZIP_DEFLATED, ZipFile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-commit", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--hardware-tested-sha256")
    args = parser.parse_args()
    if not re.fullmatch(r"[0-9a-f]{40}", args.source_commit):
        raise ValueError("Exact source commit required")
    env = "direct_biolingo_v22_psram_production"
    build = ROOT / "examples/direct_sensor/.pio/build" / env
    firmware = (build / "firmware.bin").read_bytes()
    digest = hashlib.sha256(firmware).hexdigest()
    if not 0 < len(firmware) <= 0x300000:
        raise ValueError("Unexpected application size")
    if args.hardware_tested_sha256 and digest != args.hardware_tested_sha256:
        raise ValueError("Application differs from hardware-tested binary")
    config = (ROOT / "examples/direct_sensor/src/CloudConfig.h").read_text()
    version = re.search(r'firmwareVersion\[\] = "(direct-hotspot-v[^"]+-production)"', config).group(1)
    manifest = {
        "source_commit": args.source_commit, "environment": env, "version": version,
        "board": "Biolingo v22 / ESP32-S3-WROOM-1-N16R8",
        "flash_bytes": 0x1000000, "psram_required": True,
        "distribution": "manual-usb-application-only", "endpoint": "https://green-mind.ch",
        "sample_rate": 380, "sample_bits": 16, "channels": 1,
        "calibration_version": "unsigned-mv-linear-int16-v1", "sampling_filter": "raw",
        "spool": "volatile PSRAM, up to 4 MiB; actual allocation must be checked",
        "hardware_validation": "one N16R8: 120.384s outage and 1844s continuity passed"
            if args.hardware_tested_sha256 else "software build; per-device hardware acceptance required",
        "measurement_quality": "not hardware-qualified by this release",
        "firmware": {"file": "firmware.bin", "bytes": len(firmware), "sha256": digest},
    }
    guide = (ROOT / "docs/direct-upgrade-v2.9.md").read_bytes()
    helper = (ROOT / "tools/prepare_direct_upgrade.py").read_bytes()
    args.output.mkdir(parents=True, exist_ok=True)
    target = args.output / "direct-to-cloud-production.zip"
    with ZipFile(target, "w", ZIP_DEFLATED) as package:
        package.writestr("firmware.bin", firmware)
        package.writestr("manifest.json", json.dumps(manifest, indent=2) + "\n")
        package.writestr("README.md", guide)
        package.writestr("prepare_direct_upgrade.py", helper)
    with ZipFile(target) as package:
        if set(package.namelist()) != {"firmware.bin", "manifest.json", "README.md", "prepare_direct_upgrade.py"}:
            raise ValueError("Unexpected release contents")
        if hashlib.sha256(package.read("firmware.bin")).hexdigest() != digest or package.testzip():
            raise ValueError("Release verification failed")
    (args.output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    (args.output / "SHA256SUMS").write_text(
        hashlib.sha256(target.read_bytes()).hexdigest() + "  " + target.name + "\n"
        + digest + "  firmware.bin\n")
    print(json.dumps({"package": str(target), "firmware_sha256": digest,
                      "hardware_tested_binary": bool(args.hardware_tested_sha256)}))


if __name__ == "__main__":
    main()
