# Direct-to-Cloud v2.11-rc1 avg20ms: USB bootstrap, then cloud OTA

For **Biolingo v22 / ESP32-S3-WROOM-1-N16R8**, 16 MiB flash,
8 MiB octal PSRAM, **Production / DIRECT mode** only.

- [Latest Direct ZIP](https://github.com/Dinten-dev/GreenMindArdu/releases/download/direct-latest/direct-to-cloud-production.zip)
- [Checksums](https://github.com/Dinten-dev/GreenMindArdu/releases/download/direct-latest/SHA256SUMS)
- [Version and hardware evidence](https://github.com/Dinten-dev/GreenMindArdu/releases/tag/direct-latest)

Peter must install this bootstrap **once per sensor over USB**. Old installed
Direct firmware cannot acquire OTA remotely. Afterwards the sensor polls the
existing cloud every minute and installs only a specifically assigned release.
Publishing on GitHub alone does not assign an update to every device.

The firmware produces **380 Hz mono PCM16 after a 20 ms moving average** of
forty hardware-clocked 2 kHz ADC conversions. Calibration identifier, cloud
format, PSRAM spool and NVS identity remain compatible. This changes bandwidth
and has a nominal 9.75 ms group delay; it cannot recover analog clipping.
Read the [filter and 8 MiB hardware guide](https://github.com/Dinten-dev/GreenMindArdu/blob/main/docs/direct-avg20ms.md). HPK 100 Hz is
separate processing; plant/electrode quality is not qualified by this release.

## Download, identify and privately back up

Compare the ZIP checksum with the same release's SHA256SUMS, then unzip. Use a USB
data cable and close serial monitors. Confirm the N16R8 marking and the actual
ESP32-S3 / embedded 8-MiB PSRAM / 16-MiB flash reported by esptool. Other board
variants are not covered.

```bash
python3.12 -m venv .venv
source .venv/bin/activate
python -m pip install esptool==4.11.0
python -m serial.tools.list_ports
export GM_SENSOR_PORT=/dev/cu.usbmodem101
python -m esptool --chip esp32s3 --port "$GM_SENSOR_PORT" flash_id
umask 077
GM_BACKUP_DIR=$(mktemp -d ./private-backup.XXXXXX)
python -m esptool --chip esp32s3 --port "$GM_SENSOR_PORT" --after no_reset read_flash 0x0 0x1000000 "$GM_BACKUP_DIR/flash-before.bin"
shasum -a 256 "$GM_BACKUP_DIR/flash-before.bin" > "$GM_BACKUP_DIR/SHA256SUMS"
```

Keep this backup private; it contains credentials. Never upload it to GitHub.
Wait for the upload buffer to drain before resetting; USB maintenance and OTA
reboots cause a real, identified recording gap. PSRAM does not survive resets.

## Prepare the correct USB installation

```bash
python direct-firmware/prepare_direct_ota_upgrade.py --backup "$GM_BACKUP_DIR/flash-before.bin" --port "$GM_SENSOR_PORT"
```

The helper first probes actual physical flash capacity and backup length, refusing
the 16 MiB target on 8 MiB hardware. It then verifies every firmware file and
the backed-up partition checksum.
With existing supported two-slot layouts it selects the currently valid OTA slot
from CRC-checked boot metadata and preserves the partition table, boot selection
and NVS. It prints bootloader/application write and verification commands; **it
does not write a device**. The tested bootloader supports trial-boot rollback.

Old Direct single-slot firmware needs an explicit partition migration:

```bash
python direct-firmware/prepare_direct_ota_upgrade.py --backup "$GM_BACKUP_DIR/flash-before.bin" --port "$GM_SENSOR_PORT" --install-ota-layout
```

For the supported old single-app layout at `0x10000`, this produces two 3-MiB
slots at `0x20000` and `0x320000`, preserving the original NVS address **and size**.
The previous application/filesystem remains recoverable in the private full
backup; its old layout is not retained on-device. Unknown, encrypted, corrupt,
overlapping or pending-trial layouts stop instead of guessing. Review and run
the helper's two printed commands. Do not erase the whole flash.

After boot, confirm the displayed/uploaded version, PSRAM, cloud acknowledgements
and absence of lost samples. Existing Wi-Fi and device pairing should remain.

## Cloud updates

An administrator stages the exact SHA-256 image and assigns it to a specific
device and source version using the backend's local `app.direct.ota_admin` CLI.
There is no unauthenticated control endpoint, GitHub redirect, inbound sensor
port, or fleet-wide automatic update. The device authenticates with its existing
Direct token and cloud certificate over HTTPS.

Before a flash write, recording pauses, the partial block enters the normal
queue, and **all queued blocks require their original verified acknowledgements**.
If the queue cannot drain, the update aborts and recording resumes. The binary
must fit the inactive slot and match chip, length and SHA-256; every flashed byte
is then independently read and checked before boot selection changes.

The next boot confirms only after ten successful verified uploads. A pending
trial that fails validation rolls back after 180 seconds with the included
bootloader. Crash-before-confirmation also uses bootloader rollback. This does
not provide power-loss persistence for captured PSRAM data. Planned flash/reboot
time is a maintenance gap, never fabricated measurement samples.

The ZIP's hardware-test.json records the exact tested application/bootloader and
source fingerprints. Hardware tests qualify one N16R8; other devices still need
their own installation and operational checks. The initial overlong lab version
failed ingest and rolled back; the corrected
short lab identifiers and compiler guard passed the subsequent acceptance.
USB/flash/reboot and the deliberate failed-hash test create identified gaps;
these are not uninterrupted capture claims. The older v2.9 release remains
available as an immutable manual-only package.
