import binascii
import importlib.util
import struct
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("ota_usb", ROOT / "tools/prepare_direct_ota_upgrade.py")
helper = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helper)


def boot_record(sequence, state=2):
    crc = binascii.crc32(struct.pack("<I", sequence), 0xffffffff) & 0xffffffff
    return struct.pack("<I20sII", sequence, b"\xff" * 20, state, crc)


class DirectOtaUpgradeTests(unittest.TestCase):
    def test_erased_and_legacy_boot_selectors(self):
        erased = b"\xff" * 8192
        self.assertEqual(helper.active_slot(erased), 0x20000)
        legacy = boot_record(1, 0xffffffff).ljust(4096, b"\xff") + boot_record(0, 0xffffffff).ljust(4096, b"\xff")
        self.assertEqual(helper.active_slot(legacy), 0x20000)
        selected = boot_record(1).ljust(4096, b"\xff") + boot_record(2).ljust(4096, b"\xff")
        self.assertEqual(helper.active_slot(selected), 0x320000)
        rollback = boot_record(3, 3).ljust(4096, b"\xff") + boot_record(2).ljust(4096, b"\xff")
        self.assertEqual(helper.active_slot(rollback), 0x320000)

    def test_pending_and_corrupt_selectors_stop(self):
        pending = boot_record(1, 1).ljust(8192, b"\xff")
        with self.assertRaises(ValueError):
            helper.active_slot(pending)
        corrupt = bytearray(boot_record(1).ljust(8192, b"\xff"))
        corrupt[31] ^= 1
        with self.assertRaises(ValueError):
            helper.active_slot(corrupt)

    def test_single_slot_migration_preserves_nvs_address_size_and_contents(self):
        for nvs_size in (0x4000, 0x5000, 0x6000):
            with tempfile.TemporaryDirectory() as temporary:
                directory = Path(temporary)
                for name in ("bootloader.bin", "firmware.bin", "boot_app0.bin"):
                    (directory / name).write_bytes(b"\xff" * 1024)
                # The 0x6000 layout uses metadata outside NVS, like retained deployments.
                metadata = 0xf000 if nvs_size == 0x6000 else 0xe000
                app = 0x20000 if metadata == 0xf000 else 0x10000
                # Supported old single slot is explicitly at 0x10000.
                if app != 0x10000:
                    continue
                rows = [(1, 2, 0x9000, nvs_size, b"nvs", 0),
                        (1, 0, metadata, 0x2000, b"otadata", 0),
                        (0, 0x10, app, 0x300000, b"app0", 0)]
                backup = bytearray(b"\xff" * 0x1000000)
                backup[0x8000:0x8c00] = helper.encode_table(rows)
                marker = b"private synthetic NVS contents"
                backup[0x9000:0x9000 + len(marker)] = marker
                manifest = {"files": {"firmware.bin": {"bytes": 1024}}}
                with self.assertRaisesRegex(ValueError, "explicit"):
                    helper.usb_plan(backup, manifest, directory)
                plan = helper.usb_plan(backup, manifest, directory, True)
                self.assertEqual([p[0] for p in plan], [0, 0x8000, 0xf000, 0x20000])
                replacement = (directory / "partitions-retain-nvs.bin").read_bytes().ljust(4096, b"\xff")
                new = helper.partitions(replacement)
                self.assertEqual(new[0], rows[0])
                for offset, path in plan:
                    image = path.read_bytes()
                    self.assertFalse(offset < 0x9000 + nvs_size and offset + len(image) > 0x9000)
                    backup[offset:offset + len(image)] = image
                self.assertEqual(backup[0x9000:0x9000 + len(marker)], marker)

    def test_corrupt_table_is_rejected(self):
        rows = [(1, 2, 0x9000, 0x4000, b"nvs", 0)]
        data = bytearray(helper.encode_table(rows).ljust(4096, b"\xff"))
        data[20] ^= 1
        with self.assertRaises(ValueError):
            helper.partitions(data)


if __name__ == "__main__":
    unittest.main()
