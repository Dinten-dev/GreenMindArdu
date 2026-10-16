"""Print verified USB bootstrap commands only for an unprovisioned generic sensor."""
import argparse
import hashlib
import json
import shlex
from pathlib import Path


def bootstrap_plan(directory, backup, detected):
    from flash_hardware import require_capacity
    from prepare_direct_upgrade import application_partition
    manifest = json.loads((directory / 'manifest.json').read_text())
    if (manifest.get('environment') != 'direct_biolingo_production' or
            manifest.get('distribution') != 'new-unprovisioned-usb-bootstrap-only' or
            manifest.get('flash_bytes') != 0x800000 or manifest.get('sample_rate') != 380 or
            manifest.get('endpoint') != 'https://green-mind.ch'):
        raise ValueError('Wrong bootstrap variant')
    require_capacity(detected, 0x800000, len(backup))
    # Do not migrate a sensor carrying NVS, even if it appears unpaired/offline.
    if backup[0x9000:0xe000] != b'\xff' * 0x5000:
        raise ValueError('NVS is populated: use the application-only upgrade instead')
    expected = {'bootloader.bin': 0, 'partitions.bin': 0x8000,
                'boot_app0.bin': 0xe000, 'firmware.bin': 0x10000}
    limits = {'bootloader.bin': 0x8000, 'partitions.bin': 0x1000,
              'boot_app0.bin': 0x2000, 'firmware.bin': 0x300000}
    if set(manifest['files']) != set(expected):
        raise ValueError('Unexpected bootstrap files')
    plan = []
    for name, address in expected.items():
        item = manifest['files'][name]
        data = (directory / name).read_bytes()
        if (item['offset'] != address or len(data) != item['bytes'] or
                not 0 < len(data) <= limits[name] or
                hashlib.sha256(data).hexdigest() != item['sha256']):
            raise ValueError('Bootstrap address, size or checksum mismatch')
        if name in ('bootloader.bin', 'firmware.bin') and (len(data) < 24 or
                data[0] != 0xe9 or data[12:14] != b'\x09\x00'):
            raise ValueError('Expected ESP32-S3 image')
        plan.append((address, directory / name))
    table = (directory / 'partitions.bin').read_bytes().ljust(4096, b'\xff')
    application_partition(table, manifest['files']['firmware.bin']['bytes'], 0x10000, detected)
    return plan


def main():
    from flash_hardware import probe_flash
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--backup', required=True, type=Path)
    parser.add_argument('--port', required=True)
    parser.add_argument('--new-unprovisioned', action='store_true', required=True)
    args = parser.parse_args()
    directory = Path(__file__).resolve().parent
    plan = bootstrap_plan(directory, args.backup.read_bytes(), probe_flash(args.port))
    base = ['python', '-m', 'esptool', '--chip', 'esp32s3', '--port', args.port, '--baud', '460800']
    files = [value for offset, path in plan for value in (hex(offset), str(path))]
    print('New unprovisioned sensor only. Existing NVS must be blank. No device write occurred.')
    print(shlex.join(base + ['--after', 'no_reset', 'write_flash'] + files))
    print(shlex.join(base + ['--after', 'hard_reset', 'verify_flash'] + files))


if __name__ == '__main__':
    main()
