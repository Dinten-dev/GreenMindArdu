"""Package the generic 8 MiB Direct bootstrap for unprovisioned ESP32-S3s."""
import argparse
import hashlib
import json
import re
from pathlib import Path
from zipfile import ZIP_DEFLATED, ZipFile

ROOT = Path(__file__).resolve().parents[1]
ENV = 'direct_biolingo_production'


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--source-commit', required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if not re.fullmatch(r'[0-9a-f]{40}', args.source_commit):
        raise ValueError('Exact source commit required')
    build = ROOT / 'examples/direct_sensor/.pio/build' / ENV
    files = {name: (build / name).read_bytes() for name in ('bootloader.bin', 'partitions.bin', 'firmware.bin')}
    files['boot_app0.bin'] = (Path.home() / '.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin').read_bytes()
    config = (ROOT / 'examples/direct_sensor/src/CloudConfig.h').read_text()
    version = re.search(r'firmwareVersion\[\] = "(direct-hotspot-v[^"]+-production)"', config).group(1)
    offsets = {'bootloader.bin': 0, 'partitions.bin': 0x8000, 'boot_app0.bin': 0xe000, 'firmware.bin': 0x10000}
    manifest = {'source_commit': args.source_commit, 'environment': ENV, 'version': version,
        'distribution': 'new-unprovisioned-usb-bootstrap-only', 'flash_bytes': 0x800000,
        'psram_required': False, 'endpoint': 'https://green-mind.ch', 'sample_rate': 380,
        'sample_bits': 16, 'sampling_filter': 'avg20ms', 'adc_sample_rate': 2000,
        'filter_samples': 40, 'hardware_validation': 'generic 8 MiB build tested in software; physical 8 MiB board acceptance pending',
        'files': {name: {'offset': offsets[name], 'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
                  for name, data in files.items()}}
    if len(files['firmware.bin']) > 0x300000 or len(files['bootloader.bin']) > 0x8000:
        raise ValueError('Image exceeds supported partition')
    files['manifest.json'] = (json.dumps(manifest, indent=2) + '\n').encode()
    files['README.md'] = (ROOT / 'docs/direct-avg20ms.md').read_bytes()
    for name in ('prepare_new_direct_sensor.py', 'prepare_direct_upgrade.py', 'flash_hardware.py'):
        files[name] = (ROOT / 'tools' / name).read_bytes()
    args.output.mkdir(parents=True, exist_ok=True)
    target = args.output / 'direct-new-sensor-production-8mb.zip'
    with ZipFile(target, 'w', ZIP_DEFLATED) as package:
        for name, data in files.items():
            package.writestr(name, data)
    with ZipFile(target) as package:
        if set(package.namelist()) != set(files) or package.testzip():
            raise ValueError('Package verification failed')
        for name, data in files.items():
            if package.read(name) != data:
                raise ValueError('Packaged bytes differ')
    print(json.dumps({'package': str(target), 'sha256': hashlib.sha256(target.read_bytes()).hexdigest()}))


if __name__ == '__main__':
    main()
