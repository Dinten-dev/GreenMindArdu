# Manual USB installation

For Biolingo v22, ESP32-S3-WROOM-1-N16R8, AD8232 and 16 MB flash.
Use a USB data cable and disconnect other ESP32 devices. Close serial monitors.
These instructions use Python 3.12 on macOS/Linux; choose the actual serial port reported by your machine.

## 1. Choose one package

| Choice | ZIP | Environment | Boot metadata | Application |
| --- | --- | --- | --- | --- |
| Direct Production v2.8 | `direct-production-2.8.zip` | `direct_biolingo_production` | `0xe000` | `0x10000` |
| Direct Staging v2.8 | `direct-staging-2.8.zip` | `direct_biolingo_test` | `0xe000` | `0x10000` |
| Gateway sensor v1.2.0 | `gateway-sensor-1.2.0.zip` | `biolingo_v22` | `0xf000` | `0x20000` |

All three use bootloader `0x0` and partition table `0x8000`.
Do not mix files or flash addresses. A mode/partition switch requires a reviewed backup and explicit reconfiguration.

## 2. Create a local tool environment

Install Python 3.12 from [python.org](https://www.python.org/downloads/) if needed.
Run the following in the same terminal, stopping at any failure:

```bash
GM_FLASH_DIR=$(mktemp -d)
cd "$GM_FLASH_DIR"
python3.12 -m venv .venv
source .venv/bin/activate
python -m pip install esptool==4.11.0
```

Select **exactly one** ZIP name from the table. This example chooses Direct Production:

```bash
export GM_PACKAGE=direct-production-2.8.zip
```

Download, check the ZIP against the published checksum and validate every binary before any device write:

```bash
python - <<'PY'
import hashlib, json, os
from pathlib import Path
from urllib.request import urlopen
from zipfile import ZipFile

choices = {
    'direct-production-2.8.zip': ('direct_biolingo_production', 'direct-hotspot-v2.8-production', '0xe000', '0x10000'),
    'direct-staging-2.8.zip': ('direct_biolingo_test', 'direct-hotspot-v2.8-staging', '0xe000', '0x10000'),
    'gateway-sensor-1.2.0.zip': ('biolingo_v22', '1.2.0', '0xf000', '0x20000'),
}
name = os.environ['GM_PACKAGE']
environment, version, boot_offset, app_offset = choices[name]
base = 'https://github.com/Dinten-dev/GreenMindArdu/releases/download/manual-2026-10-10/'
for filename in ('SHA256SUMS', name):
    with urlopen(base + filename, timeout=120) as response:
        Path(filename).write_bytes(response.read())
checks = {line.split()[1].lstrip('*'): line.split()[0]
          for line in Path('SHA256SUMS').read_text().splitlines() if line.strip()}
if checks.get(name) != hashlib.sha256(Path(name).read_bytes()).hexdigest():
    raise SystemExit('STOP: ZIP checksum mismatch')
expected_names = {'README.md', 'manifest.json', 'bootloader.bin', 'partitions.bin', 'boot_app0.bin', 'firmware.bin'}
with ZipFile(name) as package:
    if set(package.namelist()) != expected_names or len(package.namelist()) != len(expected_names):
        raise SystemExit('STOP: unexpected package files')
    manifest = json.loads(package.read('manifest.json'))
    if (manifest['environment'], manifest['version'], manifest['distribution']) != (environment, version, 'manual-usb-only'):
        raise SystemExit('STOP: wrong firmware variant')
    offsets = {'bootloader.bin': '0x0', 'partitions.bin': '0x8000', 'boot_app0.bin': boot_offset, 'firmware.bin': app_offset}
    if len(manifest['files']) != 4 or {f['file']: f['offset'] for f in manifest['files']} != offsets:
        raise SystemExit('STOP: wrong flash layout')
    for item in manifest['files']:
        content = package.read(item['file'])
        if len(content) != item['bytes'] or hashlib.sha256(content).hexdigest() != item['sha256']:
            raise SystemExit('STOP: binary checksum mismatch')
    package.extractall('firmware')
print('ZIP, variant, flash layout and binary checksums verified')
PY
```

## 3. Identify and back up the sensor

```bash
python -m serial.tools.list_ports
printf 'Enter the sensor serial port: '
read -r GM_SENSOR_PORT
python -m esptool --chip esp32s3 --port "$GM_SENSOR_PORT" flash_id
```

Confirm ESP32-S3 and 16 MB flash. If connection fails, hold BOOT, press RESET briefly,
release BOOT and identify the port again. Stop if the hardware differs.

Store a full flash backup privately; it can contain Wi-Fi and device credentials:

```bash
umask 077
GM_BACKUP_DIR=$(mktemp -d "$HOME/GreenMind-Sensor-Backup.XXXXXX")
python -m esptool --chip esp32s3 --port "$GM_SENSOR_PORT" --after no_reset read_flash 0x0 0x1000000 "$GM_BACKUP_DIR/flash-before.bin"
shasum -a 256 "$GM_BACKUP_DIR/flash-before.bin" > "$GM_BACKUP_DIR/SHA256SUMS"
```

Keep the backup outside the temporary firmware directory. Preserve pending recordings before switching variants.
Do not run `erase_flash`. Firmware and partition writes do not by themselves guarantee preservation across incompatible layouts.

## 4. Flash and verify

Export the selected port, then use the verified manifest's addresses:

```bash
export GM_SENSOR_PORT
cd "$GM_FLASH_DIR/firmware"
python - <<'PY'
import hashlib, json, os, subprocess, sys
from pathlib import Path
manifest = json.loads(Path('manifest.json').read_text())
args = []
for item in manifest['files']:
    data = Path(item['file']).read_bytes()
    if len(data) != item['bytes'] or hashlib.sha256(data).hexdigest() != item['sha256']:
        raise SystemExit('STOP: binary changed after verification')
    args.extend([item['offset'], item['file']])
base = [sys.executable, '-m', 'esptool', '--chip', 'esp32s3', '--port', os.environ['GM_SENSOR_PORT']]
subprocess.run(base + ['--baud', '460800', '--after', 'no_reset', 'write_flash'] + args, check=True)
subprocess.run(base + ['--after', 'hard_reset', 'verify_flash'] + args, check=True)
PY
```

Continue only when esptool verifies every file. Then follow the matching
[Direct pairing guide](direct-to-cloud.md#pair-with-the-cloud) or
[Gateway sensor guide](gateway-sensor.md#pair-with-the-gateway).
