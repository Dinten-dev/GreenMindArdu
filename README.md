# GreenMind sensor firmware

Manual USB firmware for Biolingo v22 / ESP32-S3-WROOM-1-N16R8 plant sensors using AD8232.
Choose Direct-to-Cloud for Wi-Fi upload to the cloud, or Direct-to-Gateway for upload through a Raspberry Pi.
Current downloads use local hotspot/web setup. Field hardware acceptance remains pending.

## Download and install


| Component | Download | Installation guide |
| --- | --- | --- |
| Direct-to-Cloud sensor, Production v2.8 | [Firmware ZIP](https://github.com/Dinten-dev/GreenMindArdu/releases/download/manual-2026-10-10/direct-production-2.8.zip) | [Direct USB installation](https://github.com/Dinten-dev/GreenMindArdu/blob/main/docs/direct-to-cloud.md) |
| Direct-to-Gateway sensor v1.2.0 | [Firmware ZIP](https://github.com/Dinten-dev/GreenMindArdu/releases/download/manual-2026-10-10/gateway-sensor-1.2.0.zip) | [Gateway sensor USB installation](https://github.com/Dinten-dev/GreenMindArdu/blob/main/docs/gateway-sensor.md) |
| Raspberry Pi Gateway, manual maintenance candidate | [Installer-free maintenance ZIP](https://github.com/Dinten-dev/GreenMindRPIv1/releases/download/manual-2026-10-10/gateway-maintenance-2026-10-10.zip) | [Gateway installation and maintenance](https://github.com/Dinten-dev/GreenMindRPIv1/blob/main/docs/gateway-installation.md) |
| Cloud backend and dashboard, reviewed source snapshot | [Source archive](https://github.com/Dinten-dev/GreenMindDB/releases/download/source-2026-10-10/GreenMindDB-source-2026-10-10.tar.gz) | [Cloud setup](https://github.com/Dinten-dev/GreenMindDB/blob/main/README.md#local-development) and [backend guide](https://github.com/Dinten-dev/GreenMindDB/blob/main/backend/README.md) |


Follow the matching [Direct guide](docs/direct-to-cloud.md) or [Gateway sensor guide](docs/gateway-sensor.md).
See [all downloads and checksums](docs/downloads.md). OTA is disabled in the current sensor downloads.

## Firmware choices

| Choice | Build target | Runtime |
| --- | --- | --- |
| Gateway sensor v1.2.0 | `GreenMindFirmware_Biolingo` / `biolingo_v22` | 380 Hz, existing filters, durable LittleFS spool, local Gateway code |
| Direct Production v2.8 | `examples/direct_sensor` / `direct_biolingo_production` | 380 Hz, unfiltered nominal ADC voltage, Direct cloud code |
| Direct Staging v2.8 | `examples/direct_sensor` / `direct_biolingo_test` | Separate Staging endpoint and credentials |

DUAL is an explicitly configured comparison mode, not the normal installation choice.
Its Gateway and Direct archives remain separate. Direct has about nine seconds of RAM buffering;
long outages or power loss can lose samples. The OLED reports sensor ID and last-second mean/min/max in mV.
These are amplifier output readings, not calibrated differential electrode voltages.

## Repository layout

| Directory or file | Purpose |
| --- | --- |
| `GreenMindFirmware_Biolingo/` | Active Gateway sensor project |
| `examples/direct_sensor/` | Active Direct firmware project |
| `direct_transport/` | Shared Direct transport primitives |
| `tests/`, `transport_validation/` | Native tests and Gateway transport validation |
| `tools/` | Manual release packaging |
| `docs/` | Installation and technical documentation |
| `archive/` | Historical variants and MicroPython prototype |
| `flash-sensor.sh` | Reviewed local Gateway build-and-flash wrapper |

## Development

Use Python 3.12 and PlatformIO Core 6.1.19. Build commands do not install firmware:

```bash
python3.12 -m venv .venv
source .venv/bin/activate
python -m pip install platformio==6.1.19
python -m unittest discover -s tests -v
pio run --project-dir GreenMindFirmware_Biolingo -e biolingo_v22
pio run --project-dir examples/direct_sensor -e direct_biolingo_test -e direct_biolingo_production
```

Only run `./flash-sensor.sh` after reading the Gateway installation guide and backing up the target sensor.
Native transport tests and both Direct builds run in GitHub Actions. Gateway formatting uses clang-format 18.1.8.

## Documentation and project

[Documentation index](docs/README.md) · [Gateway technical reference](docs/gateway-reference.md)
· [Direct implementation](examples/direct_sensor/README.md) · [License](LICENSE)

Research software by Traver Dinten / Galaxyadvisors AG in collaboration with FHNW.
Software checks do not replace measurement, radio-link or hardware acceptance.
