"""Protect existing sensor data and keep the durable target USB-only."""
import configparser
import csv
from pathlib import Path
import unittest

PROJECT = Path(__file__).resolve().parents[1] / "examples/direct_sensor"


class DurableLayoutTests(unittest.TestCase):
    def test_original_partitions_are_preserved_and_journal_fits_16_mib(self):
        lines = (PROJECT / "partitions_direct_flash_n16.csv").read_text().splitlines()
        rows = list(csv.reader(line for line in lines if line and not line.startswith("#")))
        regions = {r[0]: (int(r[3].strip(), 0), int(r[4].strip(), 0)) for r in rows}
        self.assertEqual({k: regions[k] for k in ("nvs", "otadata", "app0", "spiffs", "coredump")}, {
            "nvs": (0x9000, 0x5000), "otadata": (0xE000, 0x2000),
            "app0": (0x10000, 0x300000), "spiffs": (0x310000, 0xE0000),
            "coredump": (0x3F0000, 0x10000),
        })
        self.assertEqual(regions["directspool"], (0x400000, 0xC00000))
        ordered = sorted(regions.values())
        for (start, size), (following, _) in zip(ordered, ordered[1:]):
            self.assertLessEqual(start + size, following)
        self.assertEqual(sum(ordered[-1]), 16 * 1024**2)

    def test_durable_targets_do_not_replace_existing_profiles(self):
        cfg = configparser.ConfigParser(interpolation=None)
        cfg.read(PROJECT / "platformio.ini")
        test = cfg["env:direct_biolingo_n16_flash_test"]
        production = cfg["env:direct_biolingo_n16_flash_production"]
        self.assertEqual(test["board"], "biolingo_v22")
        self.assertEqual(test["board_build.partitions"], "partitions_direct_flash_n16.csv")
        self.assertIn("GREENMIND_DIRECT_DURABLE_SPOOL=1", test["build_flags"])
        self.assertNotIn("GREENMIND_CLOUD_PRODUCTION", test["build_flags"])
        self.assertEqual(production["extends"], "env:direct_biolingo_n16_flash_test")
        self.assertIn("GREENMIND_CLOUD_PRODUCTION=1", production["build_flags"])
        for name in cfg.sections():
            if name not in ("env:direct_biolingo_n16_flash_test", "env:direct_biolingo_n16_flash_production"):
                self.assertNotIn("GREENMIND_DIRECT_DURABLE_SPOOL", cfg[name].get("build_flags", ""))


if __name__ == "__main__":
    unittest.main()
