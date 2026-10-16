import hashlib
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from prepare_new_direct_sensor import bootstrap_plan
from test_direct_upgrade import table


class NewDirectSensorTests(unittest.TestCase):
    def fixture(self, root):
        image = b'\xe9' + b'\0' * 11 + b'\x09\0' + b'\0' * 1010
        data = {'bootloader.bin': image, 'firmware.bin': image,
                'boot_app0.bin': b'\xff' * 8192,
                'partitions.bin': table([(1, 2, 0x9000, 0x5000, b'nvs', 0),
                    (1, 0, 0xe000, 0x2000, b'otadata', 0),
                    (0, 0x10, 0x10000, 0x300000, b'app0', 0)])}
        offsets = {'bootloader.bin': 0, 'partitions.bin': 0x8000,
                   'boot_app0.bin': 0xe000, 'firmware.bin': 0x10000}
        manifest = {'environment': 'direct_biolingo_production',
                    'distribution': 'new-unprovisioned-usb-bootstrap-only',
                    'flash_bytes': 0x800000, 'sample_rate': 380,
                    'endpoint': 'https://green-mind.ch', 'files': {}}
        for name, body in data.items():
            (root / name).write_bytes(body)
            manifest['files'][name] = {'offset': offsets[name], 'bytes': len(body),
                                      'sha256': hashlib.sha256(body).hexdigest()}
        (root / 'manifest.json').write_text(json.dumps(manifest))

    def test_blank_backup_yields_four_writes_outside_nvs(self):
        with tempfile.TemporaryDirectory() as path:
            root = Path(path); self.fixture(root)
            plan = bootstrap_plan(root, b'\xff' * 0x800000, 0x800000)
            self.assertEqual([x[0] for x in plan], [0, 0x8000, 0xe000, 0x10000])
            for offset, file in plan:
                self.assertFalse(offset < 0xe000 and offset + file.stat().st_size > 0x9000)

    def test_populated_nvs_small_chip_wrong_backup_and_corrupt_image_refused(self):
        with tempfile.TemporaryDirectory() as path:
            root = Path(path); self.fixture(root)
            blank = b'\xff' * 0x800000
            populated = bytearray(blank); populated[0x9000] = 0
            for backup, detected in ((populated, 0x800000), (blank, 0x400000), (blank[:-1], 0x800000)):
                with self.assertRaises(ValueError):
                    bootstrap_plan(root, backup, detected)
            (root / 'firmware.bin').write_bytes(b'corrupt')
            with self.assertRaises(ValueError):
                bootstrap_plan(root, blank, 0x800000)
