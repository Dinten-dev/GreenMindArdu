# Direct v2.11-rc1: 20 ms mains average

This candidate incorporates Peter Gloor's 20 ms averaging proposal. All Direct
builds, including the generic 8 MiB target, advertise `avg20ms` in their firmware
version. Historical files retain their original version and bytes.

## Measurement change

GPIO4 / ADC1 channel 3 is sampled by the ESP32-S3 continuous ADC DMA at 2,000
conversions/s. Each output uses 40 genuine consecutive conversions. Output stays
380 Hz, mono PCM16, with the existing nominal mV calibration and cloud protocol.
OLED and upload use the same filtered values. Pairing, device identity and WLAN
credentials retain their existing namespace and storage.

A rectangular 40-point average has an ideal zero at exactly 50 Hz and harmonics
with an exact 2 kHz clock. Mains/ADC clock deviations and analog clipping limit
real suppression. Digital averaging cannot recover a saturated amplifier or ADC.
This is filtered acquisition, not an unchanged raw ADC stream or proof that
previous rail-to-rail readings were caused exclusively by mains interference.
It is also not the HPK 100 Hz processing chain.

The filter attenuates wanted signals too: approximately 0.936 amplitude at 10 Hz
and 0.757 at 20 Hz. Its nominal group delay is 9.75 ms; timestamps describe the
window's right edge. Output positions have at most one 0.5 ms raw-sample step of
quantization. Acquisition starts after a complete window. No repeated readings
are invented to compensate for scheduler stalls. DMA overflow/timeout restarts
acquisition, clears the window and records a real timing gap. OTA maintenance
also restarts with a fresh window.

Serial `adc_filter=avg20ms` diagnostics every ten seconds show real conversions,
rail readings, output frames, restarts and ADC health. A healthy nominal interval
has 20,000 conversions and 3,800 outputs. Startup partial intervals differ.
A lower filtered range alone is not evidence that analog clipping disappeared.

## Select physical hardware before flashing

| Hardware | PlatformIO target | Cloud OTA |
| --- | --- | --- |
| ESP32-S3, 8 MiB flash, generic/no PSRAM | `direct_biolingo_production` | No |
| Biolingo v22 N16R8, 16 MiB flash, 8 MiB octal PSRAM | `direct_biolingo_v22_psram_ota_production` | After accepted bootstrap |
| Same N16R8, application-only/manual | `direct_biolingo_v22_psram_production` | No |

Never install the 16 MiB partition layout on an 8 MiB sensor such as Peter's
94cc. Uploads now probe actual flash capacity before writing; a 16 MiB target on
8 MiB stops. Package preparation also checks physical capacity and full private
backup length. PSRAM variants still require actual N16R8 hardware; flash size
alone is not evidence of PSRAM. The generic target uses a limited RAM spool and
has no PSRAM outage-buffer guarantee.

## Existing sensors: application-only candidate ZIP

Install `esptool==4.11.0` in a Python virtual environment. Close serial monitors.
Run `python -m esptool --chip esp32s3 --port PORT flash_id` and privately back up
**the detected physical capacity**, 0x800000 for 8 MiB or 0x1000000 for 16 MiB,
with `read_flash 0 SIZE private-backup.bin`. The backup contains credentials;
never upload it or include it in a release.

Unzip the matching application package and run
`python prepare_direct_upgrade.py --backup PRIVATE_BACKUP --port PORT`.
For a multi-slot device, also supply its independently verified active
`--app-offset`. The helper validates package SHA, chip, flash capacity, backup
length and partition checksum, then prints application write/verification
commands. Review those commands. It does not write the sensor. Existing
bootloader, partition table, boot selection and NVS remain unchanged.
An application-only ZIP cannot turn an old single-slot installation into OTA.

## New unprovisioned sensors: 8 MiB bootstrap ZIP

Download `direct-new-sensor-production-8mb.zip` with its same-release checksum.
Make a full private physical-capacity backup, unzip, then run
`python prepare_new_direct_sensor.py --backup PRIVATE_BACKUP --port PORT --new-unprovisioned`.
The helper probes physical flash, verifies every binary/address and the partition
checksum, and requires the entire NVS area to be blank. It prints the four-file
write and verification commands; it never writes a device. Populated NVS is
refused: use the existing-device application-only package instead. This generic
8 MiB bootstrap has no PSRAM/OTA support and needs physical 8 MiB board acceptance.

## Alternative: source bootstrap

Use a private full backup first. From a checkout of this exact release commit,
install PlatformIO 6.1.19 and run
`pio run --project-dir examples/direct_sensor -e direct_biolingo_production -t upload --upload-port PORT`
for an 8 MiB device. This installs the generic bootloader, partition table and
application, and includes the physical flash guard. Use it only for new devices
or a reviewed compatible existing layout. An existing paired sensor must use
the application-only path above instead of an unreviewed partition replacement.
Set up the new device through its local portal and a matching Direct dashboard
pairing code; no identity or secret is distributed with these binaries.

N16R8 OTA bootstrap packages retain the separate physical OTA acceptance gate
and [OTA installation guide](https://github.com/Dinten-dev/GreenMindArdu/blob/main/docs/direct-upgrade.md). Until the new image passes all
OTA forward/return, boot confirmation, bad-hash and rollback checks, it remains
a candidate and must not replace `direct-latest` or be assigned fleet-wide.
The ADC-only self-test and deliberately rejecting OTA images are never downloads.

## Qualification

Host tests cover the filter notch, passband attenuation, reset/warmup, PCM,
queues, OTA guards and flash-size refusals. An attached N16R8 also ran a separate
network-free ADC probe: three complete ten-second intervals each produced
20,000 conversions and 3,800 outputs, zero ADC restarts and no observed rail
readings. The production image was restored afterwards. This probe is not a
controlled analog 50 Hz rejection measurement or a cloud/OTA acceptance.

The attached N16R8 passed verified cloud uploads, OTA forward and return with
full flash SHA readback, ten-upload boot confirmation, deliberately wrong offer
hash rejection on the physical device, and a deliberately unconfirmed trial
with automatic rollback. Test assignments were then removed and the regular
production image restored. The first overlong trial name failed HTTP 422 and
rolled back; it remains a rejected attempt, followed by the corrected successful
sequence. Version length is now checked at compile time against the native
50-character ingest limit. This is one-board OTA qualification, not a physical
8 MiB board, controlled analog rejection, outage endurance or plant test.
Firmware publication alone never assigns an update to another device.
