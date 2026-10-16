import importlib.util
from pathlib import Path
import unittest
from unittest.mock import patch
from types import SimpleNamespace

FILE = Path(__file__).resolve().parents[1] / 'tools/flash_hardware.py'
SPEC = importlib.util.spec_from_file_location('flash_hardware', FILE)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class FlashHardwareTests(unittest.TestCase):
    def test_real_8_and_16_mib_output(self):
        for size in (8, 16):
            text = f'Chip is ESP32-S3 (QFN56)\nDetected flash size: {size}MB\n'
            self.assertEqual(MODULE.parse_flash_id(text), size * 1024**2)

    def test_missing_size_wrong_chip_and_conflicting_sizes_refused(self):
        for text in ('Chip is ESP32-S3', 'Chip is ESP32\nDetected flash size: 16MB',
                     'Chip is ESP32-S3\nDetected flash size: 8MB\nDetected flash size: 16MB'):
            with self.assertRaises(ValueError):
                MODULE.parse_flash_id(text)

    def test_8_mib_sensor_cannot_receive_16_mib_bootstrap(self):
        with self.assertRaises(ValueError):
            MODULE.require_capacity(8 * 1024**2, 16 * 1024**2)

    def test_backup_must_match_physical_sensor(self):
        with self.assertRaises(ValueError):
            MODULE.require_capacity(16 * 1024**2, 8 * 1024**2, 8 * 1024**2)
        MODULE.require_capacity(16 * 1024**2, 8 * 1024**2, 16 * 1024**2)


class FlashProbeTests(unittest.TestCase):
    def test_probe_executes_only_read_only_flash_id(self):
        with patch.object(MODULE.subprocess, "run", return_value=SimpleNamespace(
                returncode=0, stdout="Chip is ESP32-S3\nDetected flash size: 8MB")) as run:
            self.assertEqual(MODULE.probe_flash("PORT", "/tool/esptool.py"), 0x800000)
        command = run.call_args.args[0]
        self.assertEqual(command[1], "/tool/esptool.py")
        self.assertEqual(command[-1], "flash_id")
        self.assertNotIn("write_flash", command)

    def test_failed_probe_never_returns_capacity(self):
        with patch.object(MODULE.subprocess, "run", return_value=SimpleNamespace(returncode=1)):
            with self.assertRaises(ValueError):
                MODULE.probe_flash("PORT")
