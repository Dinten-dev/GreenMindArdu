# Direct-to-Cloud: download, USB installation and pairing

For Biolingo v22 / ESP32-S3 with 16 MB flash and AD8232.
Current manual download: Direct v2.8. No Raspberry Pi is required.

## Download and install

[Production ZIP](https://github.com/Dinten-dev/GreenMindArdu/releases/download/manual-2026-10-10/direct-production-2.8.zip) · [SHA256SUMS](https://github.com/Dinten-dev/GreenMindArdu/releases/download/manual-2026-10-10/SHA256SUMS)
· [Release](https://github.com/Dinten-dev/GreenMindArdu/releases/tag/manual-2026-10-10).

Follow [manual USB installation](usb-installation.md) with `GM_PACKAGE=direct-production-2.8.zip`.
The manifest must identify `direct_biolingo_production` / `direct-hotspot-v2.8-production`.
OTA is not enabled. Installed devices are not automatically updated by downloading or publishing this package.

For Staging tests only, select `direct-staging-2.8.zip` and use `https://test.green-mind.ch`.
Production and Staging have separate stored identities and codes.

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

v2.8 changes the OLED to sensor ID plus mean/minimum/maximum of the latest completed sample second.
Direct acquisition, nominal voltage conversion, cloud provisioning, retry and typed acknowledgement remain unchanged.
The displayed voltage is AD8232 output, not a calibrated differential electrode reading.
Build and software checks passed; real display, hotspot and field acceptance remain pending.
Direct holds about nine seconds in RAM. Longer outages or power loss can lose data;
do not claim durable offline recording from this build.
