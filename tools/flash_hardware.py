"""Read-only ESP32-S3 flash probe shared by USB preparation and upload guards."""
import re
import subprocess
import sys


def parse_flash_id(output):
    if not re.search(r'Chip (?:is|type:)\s*ESP32-S3\b', output):
        raise ValueError('Expected an ESP32-S3; no flash command is permitted')
    sizes = re.findall(r'Detected flash size:\s*(\d+)\s*(KB|MB)', output)
    if len(sizes) != 1:
        raise ValueError('Physical flash size could not be verified')
    count, unit = sizes[0]
    return int(count) * (1024 if unit == 'KB' else 1024**2)


def probe_flash(port, tool=None):
    command = [sys.executable, tool] if tool else [sys.executable, '-m', 'esptool']
    result = subprocess.run(command + ['--chip', 'esp32s3', '--port', port,
        '--baud', '115200', 'flash_id'], capture_output=True, text=True, timeout=45)
    if result.returncode:
        raise ValueError('Read-only chip/flash probe failed; no flash command is permitted')
    return parse_flash_id(result.stdout)


def require_capacity(detected, required, backup_size=None):
    if detected < required:
        raise ValueError(f'Target requires {required // 1024**2} MiB flash; '
                         f'connected sensor has {detected // 1024**2} MiB')
    if backup_size is not None and backup_size != detected:
        raise ValueError('Private backup size differs from the connected physical flash')
