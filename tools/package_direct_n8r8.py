"""Build a manual Direct N8R8 application candidate; no hardware claims."""
import argparse
import hashlib
import json
import re
from pathlib import Path
from zipfile import ZipFile, ZIP_DEFLATED

ROOT = Path(__file__).resolve().parents[1]

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--firmware', type=Path, required=True)
    p.add_argument('--source-commit', required=True)
    p.add_argument('--output', type=Path, required=True)
    args = p.parse_args()
    if not re.fullmatch(r'[0-9a-f]{40}', args.source_commit): raise ValueError('Exact source commit required')
    firmware = args.firmware.read_bytes()
    if firmware[:1] != b'\xe9' or firmware[12:14] != b'\x09\x00' or b'direct-v2.15-rc1-raw400-live-p1-production' not in firmware or len(firmware) > 0x300000:
        raise ValueError('Wrong firmware image')
    digest = hashlib.sha256(firmware).hexdigest()
    manifest = {'source_commit': args.source_commit, 'environment': 'direct_biolingo_n8r8_production',
        'distribution': 'manual-usb-application-only', 'version': 'direct-v2.15-rc1-raw400-live-p1-production',
        'flash_bytes': 0x800000, 'psram_bytes': 0x800000, 'psram_mode': 'octal', 'ota': False,
        'endpoint': 'https://green-mind.ch', 'sample_rate': 400, 'protocol_version': 1,
        'hardware_validation': 'software build; N8R8 hardware test pending',
        'firmware': {'file': 'firmware.bin', 'bytes': len(firmware), 'sha256': digest}}
    files = {'firmware.bin': firmware, 'manifest.json': (json.dumps(manifest, indent=2)+'\n').encode(),
             'README.md': (ROOT/'docs/direct-n8r8.md').read_bytes(),
             'prepare_direct_upgrade.py': (ROOT/'tools/prepare_direct_upgrade.py').read_bytes(),
             'flash_hardware.py': (ROOT/'tools/flash_hardware.py').read_bytes()}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with ZipFile(args.output, 'w', ZIP_DEFLATED) as z:
        for name, data in files.items(): z.writestr(name, data)
    with ZipFile(args.output) as z:
        if z.testzip() or set(z.namelist()) != set(files) or any(z.read(k) != v for k, v in files.items()):
            raise ValueError('ZIP integrity failure')
    print('firmware_sha256', digest)
    print('package_sha256', hashlib.sha256(args.output.read_bytes()).hexdigest())

if __name__ == '__main__': main()
