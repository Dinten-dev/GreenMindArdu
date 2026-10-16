# Direct-to-Cloud: download, USB installation and pairing

For Biolingo v22 / ESP32-S3 with 16 MB flash and AD8232.
Current Production download: Direct v2.11-rc1 avg20ms PSRAM/OTA candidate. No Raspberry Pi is required.

## Download and install

[Latest Production ZIP](https://github.com/Dinten-dev/GreenMindArdu/releases/download/direct-latest/direct-to-cloud-production.zip)
· [SHA256SUMS](https://github.com/Dinten-dev/GreenMindArdu/releases/download/direct-latest/SHA256SUMS)
· [Current version and validation](https://github.com/Dinten-dev/GreenMindArdu/releases/tag/direct-latest).

Follow [one-time OTA USB bootstrap](direct-upgrade.md). Back up privately and retain
NVS pairing; the helper handles the supported installed flash layout.
The manifest identifies `direct_biolingo_v22_psram_ota_production` and the exact version.
Use the tested N16R8 target and verify actual PSRAM and buffer capacity afterwards.
Further cloud OTA is device-scoped and explicitly assigned by the operator.
Production and Staging identities remain separate.

## Pair with the cloud

1. Sign in to [the Production dashboard](https://green-mind.ch/de/app/sensors).
2. Select the intended organization/zone and choose **Sensor verbinden**.
3. Choose **Direkt über WLAN (Direct-Firmware)**, then **Code Erstellen**.
4. Note the fresh Direct sensor code and its displayed validity. Use one code per sensor.
5. Join the sensor hotspot **GreenMind-Sensor-XXXX** on your phone. Stay connected if it reports no internet.
6. Open `http://192.168.4.1` and confirm the page shows **PRODUCTION / green-mind.ch**.
7. Enter the 2.4 GHz Wi-Fi name/password and the Direct code. Select **Verbinden** and wait for confirmation.
8. The sensor restarts; reconnect the phone to its normal network.

These German labels match the current `/de` dashboard. A Direct code is not a Gateway code or account password.
The code is issued by the dashboard; it is never compiled into firmware.
An existing sensor using the same variant may retain its identity and resume uploading.
Hold BOOT for five seconds, then release, to reopen setup. Changing organization/zone requires matching dashboard action.

## Check actual data delivery

- OLED: LIVE/CLOUD, increasing acknowledged packets and fresh mean/min/max voltage readings.
- Loss counters should not grow continuously; `KEIN OK` is not confirmed delivery.
- Open the correct Direct sensor card and measurements in the dashboard; check recording timestamps.
- After enough data, download a WAV and confirm the expected 380 Hz mono recording.
- Observe at least ten minutes, then check a controlled restart and Wi-Fi reconnection.

Wi-Fi must provide internet, DNS and time synchronization. Direct waits for valid time before acquisition.
A listed sensor or connected status alone does not prove complete recording.
Dashboard/read-service activation is a separate server step; report missing views to the operator.

## Limits and changes

The Direct PSRAM builds store compact PCM blocks in PSRAM until an identity-, sequence- and
hash-checked upload acknowledgement. Capture continues during upload retries.
The [v2.11 avg20ms firmware](direct-avg20ms.md) averages genuine 2 kHz ADC
conversions over 20 ms before producing 380 Hz PCM16. This changes signal
bandwidth; historical files remain untouched. HPK/100-Hz processing is separate.

Historical v2.9 testing on one N16R8 passed a 120.384-second upload outage and
1,844 seconds of continuous capture across segment boundaries. That endurance
test was not repeated for v2.11; its new-image OTA and ADC checks are separately
documented in the filter guide. With 4 MiB, nominal capacity is
5,349 seconds; check actual allocation. RAM fallback is not PSRAM acceptance.
Reset/power loss clears the buffer, and finite buffers can overflow.
Other devices need their own board, PSRAM, upload and recording checks.
The public package includes the exact hardware-tested Production application.

OTA flash/reboot uses an explicit maintenance pause after all buffered blocks are
acknowledged. This is a recording gap; see the bootstrap guide's validation and
rollback contract. GitHub publication alone does not update installed sensors.
