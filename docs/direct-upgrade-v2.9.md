# Archived Direct v2.9-rc1: manual USB application upgrade

Download the current Production application for **Biolingo v22 / ESP32-S3-WROOM-1-N16R8**,
16 MiB flash and 8 MiB octal PSRAM:

- [Archived v2.9 ZIP](https://github.com/Dinten-dev/GreenMindArdu/releases/download/direct-2.9-rc1-20261013/direct-to-cloud-production.zip)
- [Archived checksums](https://github.com/Dinten-dev/GreenMindArdu/releases/download/direct-2.9-rc1-20261013/SHA256SUMS)
- [Archived Direct release](https://github.com/Dinten-dev/GreenMindArdu/releases/tag/direct-2.9-rc1-20261013)

This Direct-specific link stays the same when another component gets a release.
Read `manifest.json` for the exact version, source commit and hardware validation.
The first published package is v2.9-rc1 Production, the exact application tested on
one N16R8 sensor. It passed a 120.384-second upload outage and 1,844 seconds of
continuous capture. Other boards require their own hardware checks.

The firmware still captures **380 Hz raw nominal ADC-output mV**, mono PCM16.
HPK filtering/100 Hz downloads are separate server processing, not a sampling
change in this firmware. PSRAM keeps unacknowledged blocks during retries; its
contents disappear on reset or power loss. No OTA or automatic device update is
enabled. Wait for upload backlog to drain before disconnecting/rebooting.

## 1. Download and verify

Download the ZIP and `SHA256SUMS` from the same release. On macOS/Linux:

```bash
shasum -a 256 direct-to-cloud-production.zip
unzip direct-to-cloud-production.zip -d direct-firmware
```

Compare the ZIP digest to the published checksum before running the helper.
Its fixed `firmware.bin` is validated again against the package manifest.
The ZIP contains only the application, manifest, this guide and verification
helper; no bootloader, partition replacement, credentials or flash backup.

## 2. Identify and privately back up the sensor

Use Python 3.12 and a USB data cable. Close serial monitors and select the actual
sensor port. Confirm ESP32-S3, 16 MiB flash and N16R8 module marking.

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

Replace the example port. Keep the full backup privately: it can contain Wi-Fi
and device credentials. Never upload it to GitHub or share it in a message.

## 3. Verify the installed layout and prepare the application-only update

```bash
python direct-firmware/prepare_direct_upgrade.py --backup "$GM_BACKUP_DIR/flash-before.bin" --port "$GM_SENSOR_PORT"
```

The helper reads only the backed-up partition-table sector, validates its MD5,
partition bounds and image size, then prints two esptool commands. It does not
connect to or write a device. A single application is selected automatically.
The old Direct v2.8 `huge_app` layout normally has one app at `0x10000`.

For multiple app slots it deliberately stops. Determine the **currently selected
boot application** from the device's existing deployment records; an app label
alone does not establish which slot is active. Supply the verified active offset:

```bash
python direct-firmware/prepare_direct_upgrade.py --backup "$GM_BACKUP_DIR/flash-before.bin" --port "$GM_SENSOR_PORT" --app-offset 0x20000
```

`0x20000` was verified for our one test sensor; it is not a universal address.
If the active slot is unknown, stop and ask the operator to identify it. Do not
guess, rewrite the partition table, erase flash, or copy the old four-file USB
installation procedure into this application-only upgrade.

Run the two printed commands in order, stopping on any failure. The second
verifies the flashed image and then resets the device. Existing boot selection,
bootloader, partition table and NVS remain intact. Pairing is retained for an
existing Production Direct sensor; a different environment/mode needs the
matching Direct dashboard code and setup procedure.

## 4. Check recording and buffer capacity

Confirm Production/green-mind.ch, the new firmware version, actual PSRAM
allocation, increasing acknowledged packets, decreasing buffer fill and zero
new sample-loss counters. A 4-MiB allocation has about 89 minutes nominal capacity
at 380 Hz; use the sensor's reported actual capacity. A RAM fallback is not a
passed PSRAM installation. Check timestamp continuity and a downloaded 380-Hz
original WAV in the dashboard. Keep the private backup for rollback.

Use the [Direct pairing guide](https://github.com/Dinten-dev/GreenMindArdu/blob/main/docs/direct-to-cloud.md#pair-with-the-cloud)
if setup is required. Fresh/uninitialized devices need a separately reviewed
bootloader and partition installation; this download upgrades an existing app.
