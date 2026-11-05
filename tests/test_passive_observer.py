import importlib.util
import os
import pty
import termios
import unittest
from pathlib import Path
from unittest.mock import patch


PATH = Path(__file__).resolve().parents[1] / "tools/observe_direct.py"
SPEC = importlib.util.spec_from_file_location("observe_direct", PATH)
OBSERVER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(OBSERVER)


class PassiveObserverTests(unittest.TestCase):
    def test_real_tty_open_close_and_reopen_never_write_modem_control_lines(self):
        import fcntl

        master, slave = pty.openpty()
        name = os.ttyname(slave)
        actual_ioctl = fcntl.ioctl
        writes = []
        controls = {termios.TIOCMBIS, termios.TIOCMBIC, termios.TIOCMSET}

        def ioctl(fd, operation, *args, **kwargs):
            if operation in controls:
                writes.append(operation)
            return actual_ioctl(fd, operation, *args, **kwargs)

        try:
            with patch("fcntl.ioctl", side_effect=ioctl):
                for _ in range(2):
                    device = OBSERVER.PassiveSerial(name, 115200, timeout=0.2)
                    self.assertFalse(termios.tcgetattr(device.fileno())[2] & termios.HUPCL)
                    os.write(master, b"sensor_alive uptime_ms=900000\n")
                    self.assertEqual(device.readline(), b"sensor_alive uptime_ms=900000\n")
                    device.close()
            self.assertEqual(writes, [])
        finally:
            os.close(master)
            os.close(slave)

    def test_usb_reset_reason_is_preserved_instead_of_filtered_out(self):
        reason = OBSERVER.boot_reason("rst:0x15 (USB_UART_CHIP_RESET),boot:0xa")
        self.assertEqual(reason, {"rom_code": 21, "source": "usb_uart"})
        self.assertIsNone(OBSERVER.boot_reason("sensor_alive uptime_ms=900000"))

    def test_credentials_are_not_written_to_public_diagnostics(self):
        for value in ("password=example", "Authorization: Bearer example", "ssid=example"):
            self.assertEqual(OBSERVER.public_line(value), "[redacted diagnostic]")
        self.assertNotIn("gmd_", OBSERVER.public_line("diagnostic gmd_example"))


if __name__ == "__main__":
    unittest.main()
