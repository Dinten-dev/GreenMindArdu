"""Build explicit unqualified raw400 USB candidate archives from pinned build outputs."""
import argparse
import hashlib
import json
from pathlib import Path
from zipfile import ZipFile, ZIP_DEFLATED

ROOT = Path(__file__).resolve().parents[1]
FIRMWARE_SOURCE = 'a0dba863d1f9080924e7b27ab13cc16c13d85a9f'
SPECS = {
    'direct_biolingo_production': ('dbf5d88c1260ffdf5121e84577bd403260318380dfa4e3f2ec628ce080598ecc', 0x800000),
    'direct_biolingo_v22_psram_ota_production': ('00a6a047ee109f8c7f41cd9a9c034a35e72381b192d6d03f08b918096e3f4c1a', 0x1000000),
}

def sha(data):
    return hashlib.sha256(data).hexdigest()

def package(output, name, files):
    target = output / name
    with ZipFile(target, 'w', ZIP_DEFLATED) as z:
        for key, value in files.items(): z.writestr(key, value)
    with ZipFile(target) as z:
        if z.testzip() or set(z.namelist()) != set(files) or any(z.read(k) != v for k, v in files.items()):
            raise ValueError('ZIP integrity failure')
    return target

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build-root', required=True, type=Path)
    p.add_argument('--output', required=True, type=Path)
    p.add_argument('--boot-app0', required=True, type=Path)
    p.add_argument('--tooling-commit', required=True)
    args = p.parse_args(); args.output.mkdir(parents=True, exist_ok=True)
    if len(args.tooling_commit) != 40 or any(c not in '0123456789abcdef' for c in args.tooling_commit):
        raise ValueError('Exact tooling commit required')
    readme = (ROOT / 'docs/direct-v215-install.md').read_bytes()
    common = {name: (ROOT / 'tools' / name).read_bytes() for name in ('flash_hardware.py', 'prepare_direct_upgrade.py')}
    packages = []
    for env, (expected, flash) in SPECS.items():
        build = args.build_root / env
        files = {name: (build / name).read_bytes() for name in ('firmware.bin', 'bootloader.bin', 'partitions.bin')}
        files['boot_app0.bin'] = args.boot_app0.read_bytes()
        image = files['firmware.bin']
        if sha(image) != expected or b'direct-v2.15-rc1-raw400-live-p1-production' not in image:
            raise ValueError('Unrecognised build; refusing to package')
        manifest = {'firmware_source_commit': FIRMWARE_SOURCE, 'tooling_source_commit': args.tooling_commit, 'environment': env,
            'version': 'direct-v2.15-rc1-raw400-live-p1-production', 'flash_bytes': flash,
            'sample_rate': 400, 'sampling_filter': 'none', 'protocol_version': 1,
            'endpoint': 'https://green-mind.ch', 'hardware_validation': 'Staging rc4 live tested; these Production binaries not physically tested'}
        if flash == 0x800000:
            offsets = {'bootloader.bin': 0, 'partitions.bin': 0x8000, 'boot_app0.bin': 0xe000, 'firmware.bin': 0x10000}
            manifest.update(distribution='new-unprovisioned-usb-bootstrap-only', psram_required=False,
                files={k: {'offset': offsets[k], 'bytes': len(v), 'sha256': sha(v)} for k, v in files.items()})
            contents = {**files, **common, 'prepare_new_direct_sensor.py': (ROOT / 'tools/prepare_new_direct_sensor.py').read_bytes(), 'README.md': readme,
                        'manifest.json': (json.dumps(manifest, indent=2) + '\n').encode()}
            packages.append(package(args.output, 'direct-v2.15-new-8mb.zip', contents))
            manifest.pop('files'); manifest['distribution'] = 'manual-usb-application-only'
            manifest['firmware'] = {'file': 'firmware.bin', 'bytes': len(image), 'sha256': sha(image)}
            packages.append(package(args.output, 'direct-v2.15-app-8mb.zip', {'firmware.bin': image, **common, 'README.md': readme,
                'manifest.json': (json.dumps(manifest, indent=2) + '\n').encode()}))
        else:
            manifest.update(ota_profile='biolingo-v22-n16r8-direct-ota-v1', psram_required=True,
                files={k: {'bytes': len(v), 'sha256': sha(v)} for k, v in files.items()})
            contents = {**files, **common, 'prepare_direct_ota_upgrade.py': (ROOT / 'tools/prepare_direct_ota_upgrade.py').read_bytes(),
                        'prepare_new_direct_ota_sensor.py': (ROOT / 'tools/prepare_new_direct_ota_sensor.py').read_bytes(),
                        'README.md': readme, 'manifest.json': (json.dumps(manifest, indent=2) + '\n').encode()}
            packages.append(package(args.output, 'direct-v2.15-n16r8-ota.zip', contents))
    (args.output / 'INSTALLER-SHA256SUMS').write_text(''.join(sha(p.read_bytes()) + '  ' + p.name + '\n' for p in packages))
    print('\n'.join(str(p) for p in packages))

if __name__ == '__main__': main()
