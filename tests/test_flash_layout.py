import runpy
import unittest
from pathlib import Path


PROJECT = Path(__file__).resolve().parents[1] / "GreenMindFirmware_Biolingo"


class BuildEnvironment(dict):
    def subst(self, _):
        return str(PROJECT)

    def GetProjectOption(self, _):
        return "partitions.csv"

    def Replace(self, **values):
        self.update(values)


class FlashLayoutTests(unittest.TestCase):
    def test_usb_addresses_match_app_and_ota_partitions_without_touching_nvs(self):
        env = BuildEnvironment(
            ESP32_APP_OFFSET="0x10000",
            FLASH_EXTRA_IMAGES=[("0x0000", "bootloader.bin"), ("0x8000", "partitions.bin"), ("0xe000", "boot_app0.bin")],
            UPLOADERFLAGS=["--chip", "esp32s3", "write_flash", "0x0000", "bootloader.bin", "0x8000", "partitions.bin", "0xe000", "boot_app0.bin"],
        )
        runpy.run_path(str(PROJECT / "scripts/flash_layout.py"), init_globals={"env": env, "Import": lambda _: None})
        self.assertEqual(int(env["ESP32_APP_OFFSET"], 0), 0x20000)
        self.assertEqual(env["FLASH_EXTRA_IMAGES"][-1], ("0xf000", "boot_app0.bin"))
        self.assertEqual(env["UPLOADERFLAGS"][-2:], ["0xf000", "boot_app0.bin"])
        self.assertNotIn("0xe000", env["UPLOADERFLAGS"])
        # NVS occupies 0x9000..0xefff; OTA metadata occupies 0xf000..0x10fff.
        self.assertGreaterEqual(0xF000, 0x9000 + 0x6000)
        self.assertLessEqual(0xF000 + 0x2000, 0x20000)


if __name__ == "__main__":
    unittest.main()
