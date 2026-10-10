"""Package already-built manual firmware; never flashes or publishes OTA."""
import argparse
import hashlib
import json
import re
from pathlib import Path
from zipfile import ZIP_DEFLATED, ZipFile

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument("--source-commit", required=True)
parser.add_argument("--boot-app", type=Path, required=True)
parser.add_argument("--output", type=Path, required=True)
args = parser.parse_args()
assert re.fullmatch(r"[0-9a-f]{40}", args.source_commit)
assert args.boot_app.name == "boot_app0.bin"
args.output.mkdir(parents=True, exist_ok=True)
specs = [
    ("gateway-sensor-1.2.0", "GreenMindFirmware_Biolingo", "biolingo_v22", "1.2.0", "0xf000", "0x20000"),
    ("direct-production-2.8", "examples/direct_sensor", "direct_biolingo_production", "direct-hotspot-v2.8-production", "0xe000", "0x10000"),
    ("direct-staging-2.8", "examples/direct_sensor", "direct_biolingo_test", "direct-hotspot-v2.8-staging", "0xe000", "0x10000"),
]
checksums = []
for name, project, env, version, ota_offset, app_offset in specs:
    build = ROOT / project / ".pio/build" / env
    files = [(build / "bootloader.bin", "0x0"), (build / "partitions.bin", "0x8000"),
             (args.boot_app, ota_offset), (build / "firmware.bin", app_offset)]
    assert files[-1][0].stat().st_size <= 0x300000
    manifest = {"source_commit": args.source_commit, "environment": env, "version": version,
                "board": "Biolingo v22 ESP32-S3", "distribution": "manual-usb-only",
                "hardware_validation": "field acceptance pending", "files": []}
    path = args.output / (name + ".zip")
    with ZipFile(path, "w", ZIP_DEFLATED) as package:
        for file, offset in files:
            content = file.read_bytes()
            manifest["files"].append({"file": file.name, "offset": offset, "bytes": len(content), "sha256": hashlib.sha256(content).hexdigest()})
            package.writestr(file.name, content)
        package.writestr("manifest.json", json.dumps(manifest, indent=2) + "\n")
        package.writestr("README.md", f"""# {name}

Manual USB installation only. No OTA or automatic update of existing sensors.
Source: {args.source_commit}
Environment: {env}

Verify ZIP against SHA256SUMS, then every binary against manifest.json.
Flash addresses are specific to this variant; never mix Gateway and Direct.
Preserve existing NVS/spooled recordings; never erase flash implicitly.
Use the matching setup guide in docs at the source commit.

OLED: sensor ID plus last-second mean/minimum/maximum in mV.
Sampling and upload payloads remain unchanged.
Build and automated checks passed; physical field acceptance remains pending.
""")
    # Validate the exact public package, not only its build inputs.
    with ZipFile(path) as package:
        checked = json.loads(package.read("manifest.json"))
        assert len(package.namelist()) == 6
        for file in checked["files"]:
            content = package.read(file["file"])
            assert len(content) == file["bytes"]
            assert hashlib.sha256(content).hexdigest() == file["sha256"]
    checksums.append(hashlib.sha256(path.read_bytes()).hexdigest() + "  " + path.name)
    print(path.name)
(args.output / "SHA256SUMS").write_text("\n".join(checksums) + "\n")
