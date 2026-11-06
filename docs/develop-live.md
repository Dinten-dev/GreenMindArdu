# Develop: Direct raw400 and Staging Live

Updated 6 November 2026. `develop` is the shared integration branch for the
current source. The corresponding cloud source is
[GreenMindDB develop](https://github.com/Dinten-dev/GreenMindDB/tree/develop).
Its [integration guide](https://github.com/Dinten-dev/GreenMindDB/blob/develop/docs/DEVELOP-LIVE.md)
describes server roles and rollout boundaries.

## Source, running candidate and downloads

| Item | Meaning |
| --- | --- |
| Current Direct source | v2.15, nominal raw400 with on-demand Live |
| Staging default identity | `direct-v2.15-rc4-raw400-live-p1-staging` |
| Default protocol | v1; v2 remains explicitly opt-in |
| Published Direct package | v2.11-rc1 avg20ms, 380 Hz; inspect its release manifest |
| Gateway and DUAL | Existing 380-Hz path and separate credentials/archives |

Live was used on separate Staging hardware. The user confirmed operation and
requested testing to stop. Network unavailability affected the run; it is not
a completed endurance or physical-touch latency acceptance. No new field
binary, public download replacement or office-device OTA follows from merging
this source. The [raw400 acceptance criteria](direct-raw400.md) remain applicable.

## Build without flashing

Use Python 3.12, PlatformIO Core 6.1.19 and the pinned project dependencies:

```bash
git clone --branch develop https://github.com/Dinten-dev/GreenMindArdu.git
cd GreenMindArdu
python3.12 -m venv .venv
source .venv/bin/activate
python -m pip install platformio==6.1.19 pyserial==3.5
python -m unittest discover -s tests -v
pio run --project-dir examples/direct_sensor -e direct_biolingo_v22_psram_test
```

The N16R8 target requires physical ESP32-S3 hardware with 16 MiB flash and
8 MiB octal PSRAM. Generic targets are separate; never infer physical capacity
from a build target. These commands compile and run host tests only.
They do not write a device or deploy a server. CI covers seven Direct targets,
the existing Gateway firmware and native transport/acquisition tests.

## Installation and pairing boundaries

1. Deploy and verify a compatible cloud receiver and private Live owner first.
   Keep `DIRECT_LIVE_ENABLED` explicit and use the same private socket for its APIs.
2. Choose an authorized, separate test sensor and the matching hardware target.
   Confirm its existing partition layout before any application-only upgrade.
   Preserve its environment-specific NVS identity and credentials.
3. Staging firmware uses `test.green-mind.ch`. Pair only with a Direct code
   from that environment; Production and Gateway codes are different credentials.
4. Configure a reachable 2.4-GHz WLAN or phone hotspot using the existing setup
   portal. Remote viewers need internet access to Staging, not the sensor's WLAN.
5. Open Measurements, select the Direct sensor, then explicitly select Live.
   New measurement views default to 24 hours. Live offers 30/60/180-second windows,
   rail diagnostics and markers retained only in volatile memory for three minutes.

The published [v2.11 USB/OTA guide](direct-upgrade.md) belongs to its own package
and manifest; its flash layout or commands must not be blindly applied to a
locally compiled raw400 image. `pio run` alone is not an installation procedure.
Device updates remain explicitly authorized maintenance actions.

## Connections and diagnostics

Raw capture and acknowledgement retries are independent of viewer demand.
Live uses one shared TLS worker, a bounded preview queue and immutable packet
retries. A network outage can pause Live, age the rail measurement or leave
real gaps after the finite preview retention expires. Missing values are not
interpolated. Raw PSRAM retention is separate and does not survive reset.
See the exact [Live budgets and contract](direct-demand-live.md).

Use the [passive observer](passive-usb-observation.md), not an automatically
reopening serial monitor. Opening a conventional monitor can reset the USB
Serial/JTAG sensor. The observer never writes modem-control lines and stops
on a disconnected port. Choose the actual port and new private output paths:

```bash
umask 077
mkdir -p private-observation
python tools/observe_direct.py --port /dev/cu.usbmodem101 --duration 300 --events private-observation/events.jsonl --stop-file private-observation/stop
```

The example port must be replaced with the identified sensor's port. Stop by
creating the configured stop file or interrupting the observer; neither action
requests a device reset. Keep credentials, full serial logs and local test
receipts out of commits and public PR descriptions.
