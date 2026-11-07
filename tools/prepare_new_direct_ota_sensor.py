"""Prepare a blank N16R8 for Direct OTA. Never write to the device."""
import argparse
import shlex
from pathlib import Path
from flash_hardware import probe_flash, require_capacity
from prepare_direct_ota_upgrade import partitions, verify_package


def bootstrap_plan(directory, backup, detected):
    manifest = verify_package(directory)
    require_capacity(detected, 0x1000000, len(backup))
    if backup != b'\xff' * len(backup):
        raise ValueError('New sensor bootstrap requires completely blank flash')
    rows = partitions((directory / 'partitions.bin').read_bytes().ljust(4096, b'\xff'))
    expected = {(0, 0x10, 0x20000, 0x300000, b'app0', 0),
                (0, 0x11, 0x320000, 0x300000, b'app1', 0),
                (1, 0, 0xf000, 0x2000, b'otadata', 0),
                (1, 2, 0x9000, 0x6000, b'nvs', 0)}
    if not expected.issubset(set(rows)) or manifest['files']['firmware.bin']['bytes'] > 0x300000:
        raise ValueError('Unexpected OTA partition layout')
    return [(0, directory / 'bootloader.bin'), (0x8000, directory / 'partitions.bin'),
            (0xf000, directory / 'boot_app0.bin'), (0x20000, directory / 'firmware.bin')]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--backup', type=Path, required=True)
    parser.add_argument('--port', required=True)
    parser.add_argument('--new-unprovisioned', action='store_true', required=True)
    args = parser.parse_args()
    directory = Path(__file__).resolve().parent
    plan = bootstrap_plan(directory, args.backup.read_bytes(), probe_flash(args.port))
    print('Confirm N16R8 hardware: 16 MiB flash and 8 MiB PSRAM. No device write occurred.')
    base = ['python', '-m', 'esptool', '--chip', 'esp32s3', '--port', args.port, '--baud', '460800']
    files = [value for offset, path in plan for value in (hex(offset), str(path))]
    print(shlex.join(base + ['--after', 'no_reset', 'write_flash'] + files))
    print(shlex.join(base + ['--after', 'hard_reset', 'verify_flash'] + files))

if __name__ == '__main__':
    main()
