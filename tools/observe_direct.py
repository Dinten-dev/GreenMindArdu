"""Read Direct diagnostics without resetting the USB Serial/JTAG sensor.

Never write DTR/RTS and never reopen a disconnected port automatically. PySerial's
normal open applies modem lines separately; on ESP32-S3 that can issue a USB UART
chip reset. HUPCL is cleared so closing this observer does not drop modem lines.
"""

import argparse
import json
import os
import re
import signal
import termios
import time
from pathlib import Path

import serial


class PassiveSerial(serial.Serial):
    def _update_dtr_state(self):
        pass

    def _update_rts_state(self):
        pass

    def open(self):
        super().open()
        try:
            attrs = termios.tcgetattr(self.fileno())
            attrs[2] &= ~termios.HUPCL
            termios.tcsetattr(self.fileno(), termios.TCSANOW, attrs)
        except Exception:
            self.close()
            raise


def public_line(line):
    if re.search(r"password|authorization|\btoken\b|\bssid\b|https?://", line, re.I):
        return "[redacted diagnostic]"
    return re.sub(r"gmd_[A-Za-z0-9_-]+", "[redacted]", line)


def boot_reason(line):
    match = re.search(r"\brst:0x([0-9a-fA-F]+)", line)
    if not match:
        return None
    code = int(match[1], 16)
    return {"rom_code": code, "source": {21: "usb_uart", 22: "usb_jtag"}.get(code, "other")}


class DiagnosticLines:
    """Keep timeout-split serial lines intact before redaction or parsing."""

    def __init__(self):
        self.pending = bytearray()
        self.discarding = False

    def feed(self, data):
        lines = []
        for value in data:
            if value == 10:
                line = "[oversized diagnostic discarded]" if self.discarding else self.pending.decode(errors="replace").strip()
                if line:
                    lines.append(line)
                self.pending.clear()
                self.discarding = False
            elif not self.discarding:
                if len(self.pending) == 8192:
                    self.pending.clear()
                    self.discarding = True
                else:
                    self.pending.append(value)
        return lines


def observe(port, duration, events, private=None, stop_file=None):
    device = PassiveSerial(port=None, baudrate=115200, timeout=0.2, write_timeout=3)
    device.port = port
    device.open()
    deadline = time.monotonic() + duration
    stopped = False

    def stop(*_):
        nonlocal stopped
        stopped = True

    old_handlers = {sig: signal.signal(sig, stop) for sig in (signal.SIGTERM, signal.SIGINT)}

    def event(value):
        value["at"] = time.time()
        events.write(json.dumps(value) + "\n")
        events.flush()

    event({"event": "opened", "modem_lines_written": False, "automatic_reopen": False})
    lines = DiagnosticLines()
    try:
        while not stopped and time.monotonic() < deadline:
            if stop_file and stop_file.exists():
                break
            try:
                fragment = device.read_until(b"\n", size=2048)
            except (serial.SerialException, OSError) as error:
                event({"event": "disconnected", "error_type": type(error).__name__})
                return 1
            for line in lines.feed(fragment):
                if private:
                    private.write(json.dumps({"at": time.time(), "line": line}) + "\n")
                    private.flush()
                value = {"event": "serial", "line": public_line(line)}
                reason = boot_reason(line)
                if reason:
                    value["reset"] = reason
                event(value)
    finally:
        device.close()
        event({"event": "closed", "automatic_reopen": False})
        for sig, handler in old_handlers.items():
            signal.signal(sig, handler)
    return 0


def new_private_file(path):
    return os.fdopen(os.open(path, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600), "w")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--duration", type=int, default=300)
    parser.add_argument("--events", type=Path, required=True)
    parser.add_argument("--private-log", type=Path)
    parser.add_argument("--stop-file", type=Path)
    args = parser.parse_args()
    if not 1 <= args.duration <= 86400:
        parser.error("Duration must be between one second and one day")
    with new_private_file(args.events) as events:
        if args.private_log:
            with new_private_file(args.private_log) as private:
                return observe(args.port, args.duration, events, private, args.stop_file)
        return observe(args.port, args.duration, events, stop_file=args.stop_file)


if __name__ == "__main__":
    raise SystemExit(main())
