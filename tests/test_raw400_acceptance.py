import importlib.util
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("capture_check", ROOT / "tools/check_raw400_capture.py")
check = importlib.util.module_from_spec(spec)
spec.loader.exec_module(check)

WINDOW = ("adc_profile=ad8232-raw400-v1 target_hz=400 interval_us=2500 "
          "raw_samples=4000 raw_rail=3200 window_us=10000000 window_start_us=1000 window_end_us=10001000 raw_invalid=0 "
          "scale_clipped=0 missed_slots=0 duplicate_wakeups=0 queue_dropped=0 "
          "max_adc_us=30 max_callback_us=60 max_jitter_us=100 max_interval_us=2650 "
          "heap=110000 min_heap=100000")


class Raw400AcceptanceTests(unittest.TestCase):
    def test_high_rail_is_reportable_but_not_a_fabricated_sampling_failure(self):
        result = check.inspect_capture(WINDOW, minimum_seconds=10)
        self.assertTrue(result["capture_criteria_met"])
        self.assertFalse(result["release_authorized"])
        self.assertEqual(result["observed_min_hz"], 400)

    def test_rate_loss_timing_memory_and_missing_data_fail_acceptance(self):
        for before, after in [("raw_samples=4000", "raw_samples=3800"),
                              ("missed_slots=0", "missed_slots=1"),
                              ("queue_dropped=0", "queue_dropped=10"),
                              ("scale_clipped=0", "scale_clipped=1"),
                              ("max_jitter_us=100", "max_jitter_us=1000"),
                              ("max_callback_us=60", "max_callback_us=2500"),
                              ("min_heap=100000", "min_heap=20000"),
                              ("window_us=10000000", "window_us=0")]:
            with self.subTest(after=after):
                self.assertFalse(check.inspect_capture(WINDOW.replace(before, after),
                                                       minimum_seconds=10)["capture_criteria_met"])
        self.assertFalse(check.inspect_capture(WINDOW + "\n" + WINDOW,
                                               minimum_seconds=20)["capture_criteria_met"])
        self.assertFalse(check.inspect_capture(WINDOW + "\ndirect_dropped=1 timing_dropped=0",
                                               minimum_seconds=10)["capture_criteria_met"])
        self.assertFalse(check.inspect_capture("")["capture_criteria_met"])
        self.assertFalse(check.inspect_capture(WINDOW)["capture_criteria_met"])

    def test_legacy_release_packagers_cannot_mislabel_a_raw400_candidate(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "release"
            report = Path(directory) / "old-proof.json"
            report.write_text("{}")
            for script in ("package_direct_release.py", "package_direct_usb_bootstrap.py",
                           "package_direct_ota_release.py"):
                args = [sys.executable, str(ROOT / "tools" / script),
                        "--source-commit", "a" * 40, "--output", str(output)]
                if "ota" in script:
                    args += ["--hardware-report", str(report)]
                result = subprocess.run(args, capture_output=True, text=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("legacy 380 Hz packaging disabled", result.stderr)
                self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
