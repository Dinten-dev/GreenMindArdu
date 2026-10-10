# Direct-to-Gateway sensor: download, USB installation and pairing

For Biolingo v22 / ESP32-S3-WROOM-1-N16R8 and AD8232. Current manual sensor firmware: v1.2.0.
This variant sends measurements to a registered Raspberry Pi Gateway.

## Download and install

[Gateway sensor ZIP](https://github.com/Dinten-dev/GreenMindArdu/releases/download/manual-2026-10-10/gateway-sensor-1.2.0.zip) · [SHA256SUMS](https://github.com/Dinten-dev/GreenMindArdu/releases/download/manual-2026-10-10/SHA256SUMS)
· [Release](https://github.com/Dinten-dev/GreenMindArdu/releases/tag/manual-2026-10-10).

Follow [manual USB installation](usb-installation.md) with `GM_PACKAGE=gateway-sensor-1.2.0.zip`.
The manifest must identify `biolingo_v22` / `1.2.0`, metadata at `0xf000` and application at `0x20000`.
Do not use Direct flash addresses. Preserve NVS and queued readings; do not erase flash.

Alternatively, build a reviewed source checkout and run `./flash-sensor.sh --port YOUR_ACTUAL_PORT`.
That wrapper builds the Gateway variant only and writes to the chosen sensor.

## Pair with the Gateway

1. Install/register the Pi using [the Gateway guide](https://github.com/Dinten-dev/GreenMindRPIv1/blob/main/docs/gateway-installation.md).
2. On the Pi's local `/api/v1/health`, confirm `ingest_protocol_versions: [1, 2, 3]` and `sequence_acknowledgement: true` before using this protocol-v3 sensor.
3. In the dashboard, generate a **Gateway sensor verification code** for the intended Gateway and zone.
4. Join the sensor's **GreenMind-Sensor-XXXX** hotspot and open `http://192.168.4.1`.
5. Enter the 2.4 GHz Wi-Fi credentials and that Gateway sensor code.
6. Confirm sensor registration and acknowledged data reception on the Gateway.

Gateway registration and individual sensor registration use different codes.
Use the local sensor web form; this download does not use BLE provisioning.
Hold BOOT for five seconds and release to reopen setup. Existing credential keys and pending spool remain.
Ensure sensor and Pi can communicate on the local network; independent sensor USB power avoids shared-ground interference.

## Acceptance and changes

- Check at least ten minutes of sensor acknowledgements, the correct dashboard zone and a downloadable 380 Hz WAV.
- Check reconnect and controlled restart while retaining pending measurements.
- OLED shows sensor ID and mean/min/max of the last completed second. This helps diagnosis, not radio-loss recovery.
- Local web/code setup replaces the obsolete BLE path in this manual release.
- OTA checks are disabled. Sampling, filters, spool, transport and acknowledgements are retained.

WAV close diagnostics and WSS configuration belong to the separate Pi maintenance candidate.
Server ingestion improvements do not require this sensor update.
Software checks passed; actual hotspot, display, verification and radio operation require field acceptance.
