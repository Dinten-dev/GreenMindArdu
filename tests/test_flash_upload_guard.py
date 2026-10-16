"""Exercise the actual PlatformIO upload hook with a fake physical probe."""
import runpy
import sys
import unittest
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import flash_hardware


class UploadEnvironment:
    def __init__(self):
        self.hook = None

    def subst(self, value):
        return {'$PROJECT_DIR': str(ROOT / 'examples/direct_sensor'),
                '$UPLOAD_PORT': 'TEST_USB_PORT', '$UPLOADER': '/esptool.py'}[value]

    def AddPreAction(self, target, hook):
        assert target == 'upload'
        self.hook = hook

    def AutodetectUploadPort(self):
        pass

    def BoardConfig(self):
        return {'upload.flash_size': '16MB'}


class FlashUploadGuardTests(unittest.TestCase):
    def test_actual_upload_hook_refuses_an_8_mib_board_before_upload(self):
        env = UploadEnvironment()
        runpy.run_path(str(ROOT / 'examples/direct_sensor/guard_flash.py'),
                       init_globals={'Import': lambda _: None, 'env': env})
        self.assertIsNotNone(env.hook)
        with patch.object(flash_hardware, 'probe_flash', return_value=0x800000) as probe:
            # guard_flash imports the function, so patch its execution globals.
            env.hook.__globals__['probe_flash'] = probe
            with self.assertRaisesRegex(ValueError, 'requires 16 MiB'):
                env.hook(None, None, env)
            probe.assert_called_once_with('TEST_USB_PORT', '/esptool.py')
