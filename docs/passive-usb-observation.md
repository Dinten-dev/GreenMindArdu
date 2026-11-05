# Observe USB diagnostics without resetting the sensor

Use `tools/observe_direct.py` with an explicit port, duration and new event file.
It reads only: DTR/RTS update operations are suppressed and HUPCL is cleared so
closing the observer does not drop modem lines. A disconnected port ends the
observation; it is never silently reopened.

Ordinary PySerial opening applies DTR and RTS separately. On the ESP32-S3 USB
Serial/JTAG interface, this can itself cause `USB_UART_CHIP_RESET` (ROM reason
`0x15`). Reopening automatically after a disconnect can repeat that reset. A new
session/zero sequence by itself therefore does not identify a firmware crash.

The observer records boot/reset lines instead of filtering them out. Events
redact credential-like lines and use mode 0600; an optional separate private log
retains the complete transcript for local diagnosis. Do not publish private
transcripts or network credentials. Check uninterrupted uptime and the same
server-side session before and after opening/closing the observer.

The actual C++ sensor firmware remains rc3 for this host-tool correction. No
firmware flash or saved WLAN/cloud-identity change is needed to change observers.
The pseudo-terminal regression test checks opening, reading, closing and reopening
without any modem-line write and verifies that HUPCL stays cleared.
