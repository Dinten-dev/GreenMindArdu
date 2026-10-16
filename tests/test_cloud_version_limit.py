"""Compile the actual firmware identity against the cloud's 50-character limit."""
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class CloudVersionLimitTests(unittest.TestCase):
    def test_exact_limit_compiles_but_oversize_ota_identity_is_refused(self):
        source = '#include "' + str(ROOT / 'examples/direct_sensor/src/CloudConfig.h') + '"\n'
        with tempfile.TemporaryDirectory() as path:
            for length in (50, 51):
                result = subprocess.run(['g++', '-std=c++17', '-DGREENMIND_CLOUD_PRODUCTION=1',
                    '-DGREENMIND_DIRECT_OTA=1', '-DGREENMIND_DIRECT_OTA_VERSION="' + 'x' * length + '"',
                    '-x', 'c++', '-c', '-o', str(Path(path) / 'version.o'), '-'], input=source,
                    text=True, capture_output=True)
                self.assertEqual(result.returncode == 0, length == 50, result.stderr)
                if length == 51:
                    self.assertIn('exceeds cloud protocol limit', result.stderr)
