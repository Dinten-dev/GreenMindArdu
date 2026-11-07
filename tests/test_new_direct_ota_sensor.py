import hashlib
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from prepare_direct_ota_upgrade import encode_table
from prepare_new_direct_ota_sensor import bootstrap_plan

class NewOtaSensorTests(unittest.TestCase):
    def test_blank_n16r8_only_and_corrupt_image_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            image = b'\xe9' + b'\x00' * 11 + b'\x09\x00' + b'\x00' * 1010
            rows = [(1, 2, 0x9000, 0x6000, b'nvs', 0), (1, 0, 0xf000, 0x2000, b'otadata', 0),
                    (0, 0x10, 0x20000, 0x300000, b'app0', 0), (0, 0x11, 0x320000, 0x300000, b'app1', 0)]
            files = {'firmware.bin': image, 'bootloader.bin': image,
                     'partitions.bin': encode_table(rows), 'boot_app0.bin': b'\xff' * 8192}
            manifest = {'environment': 'direct_biolingo_v22_psram_ota_production',
                        'ota_profile': 'biolingo-v22-n16r8-direct-ota-v1',
                        'sample_rate': 400, 'endpoint': 'https://green-mind.ch', 'files': {}}
            for name, data in files.items():
                (root / name).write_bytes(data)
                manifest['files'][name] = {'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
            (root / 'manifest.json').write_text(json.dumps(manifest))
            blank = b'\xff' * 0x1000000
            self.assertEqual([offset for offset, _ in bootstrap_plan(root, blank, 0x1000000)],
                             [0, 0x8000, 0xf000, 0x20000])
            occupied = bytearray(blank); occupied[0x9000] = 0
            for backup, size in ((occupied, 0x1000000), (blank, 0x800000), (blank[:-1], 0x1000000)):
                with self.assertRaises(ValueError): bootstrap_plan(root, backup, size)
            (root / 'firmware.bin').write_bytes(b'corrupt')
            with self.assertRaises(ValueError): bootstrap_plan(root, blank, 0x1000000)
