import hashlib
import importlib.util
import struct
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("upgrade", ROOT / "tools/prepare_direct_upgrade.py")
upgrade = importlib.util.module_from_spec(spec)
spec.loader.exec_module(upgrade)


def table(parts):
    raw = b"".join(struct.pack("<2sBBLL16sL", b"\xaa\x50", *p) for p in parts)
    raw += b"\xeb\xeb" + b"\xff" * 14 + hashlib.md5(raw, usedforsecurity=False).digest()
    return raw.ljust(4096, b"\xff")


class DirectUpgradeTests(unittest.TestCase):
    def test_single_app_layout_leaves_nvs_and_boot_metadata_outside_write(self):
        data = table([(1, 2, 0x9000, 0x5000, b"nvs", 0),
                      (1, 0, 0xe000, 0x2000, b"otadata", 0),
                      (0, 0x10, 0x10000, 0x300000, b"app0", 0)])
        self.assertEqual(upgrade.application_partition(data, 1044912), 0x10000)

    def test_multi_slot_requires_verified_active_offset(self):
        data = table([(0, 0x10, 0x20000, 0x300000, b"app0", 0),
                      (0, 0x11, 0x320000, 0x300000, b"app1", 0)])
        with self.assertRaisesRegex(ValueError, "Multiple"):
            upgrade.application_partition(data, 1000000)
        self.assertEqual(upgrade.application_partition(data, 1000000, 0x20000), 0x20000)
        self.assertEqual(upgrade.application_partition(data, 1000000, 0x320000), 0x320000)
        with self.assertRaises(ValueError):
            upgrade.application_partition(data, 1000000, 0x10000)

    def test_corruption_truncation_oversize_overlap_and_encryption_fail_closed(self):
        valid = table([(0, 0x10, 0x10000, 0x300000, b"app0", 0)])
        bad = bytearray(valid); bad[20] ^= 1
        for data, size in [(bytes(bad), 100), (valid[:-1], 100), (valid, 0x300001),
                           (table([(0, 0x10, 0x10000, 0x300000, b"app", 1)]), 100),
                           (table([(0, 0x10, 0x10000, 0x300000, b"app", 0),
                                   (1, 2, 0x10000, 4096, b"nvs", 0)]), 100)]:
            with self.subTest(size=size):
                with self.assertRaises(ValueError):
                    upgrade.application_partition(data, size)


if __name__ == "__main__":
    unittest.main()
