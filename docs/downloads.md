# Downloads and installation

The [v2.11 avg20ms release candidate](direct-avg20ms.md) adds hardware-clocked 20 ms
averaging, an 8 MiB new-sensor bootstrap and a physical flash-size guard.
The exact N16R8 OTA image passed the attached-sensor sequence; generic 8 MiB
physical board acceptance remains pending.

Direct downloads updated 16 October 2026; other components retain their reviewed 10 October versions. Direct OTA needs one-time USB bootstrap; Gateway firmware remains manual USB.
The Raspberry Pi package is a manual maintenance candidate with hardware acceptance pending.
Cloud source publication does not deploy a server or activate an archive policy.


| Component | Download | Installation guide |
| --- | --- | --- |
| Direct-to-Cloud sensor, Production v2.11-rc1 / PSRAM + OTA candidate | [Firmware ZIP](https://github.com/Dinten-dev/GreenMindArdu/releases/download/direct-latest/direct-to-cloud-production.zip) | [Direct USB upgrade](https://github.com/Dinten-dev/GreenMindArdu/blob/main/docs/direct-upgrade.md) |
| New unprovisioned Direct sensor, generic 8 MiB | [Bootstrap ZIP](https://github.com/Dinten-dev/GreenMindArdu/releases/download/direct-2.11-rc1-20261016/direct-new-sensor-production-8mb.zip) | [8 MiB setup and limits](direct-avg20ms.md) |
| Direct-to-Gateway sensor v1.2.0 | [Firmware ZIP](https://github.com/Dinten-dev/GreenMindArdu/releases/download/manual-2026-10-10/gateway-sensor-1.2.0.zip) | [Gateway sensor USB installation](https://github.com/Dinten-dev/GreenMindArdu/blob/main/docs/gateway-sensor.md) |
| Raspberry Pi Gateway, manual maintenance candidate | [Installer-free maintenance ZIP](https://github.com/Dinten-dev/GreenMindRPIv1/releases/download/manual-2026-10-10/gateway-maintenance-2026-10-10.zip) | [Gateway installation and maintenance](https://github.com/Dinten-dev/GreenMindRPIv1/blob/main/docs/gateway-installation.md) |
| Cloud backend and dashboard, reviewed source snapshot | [Source archive](https://github.com/Dinten-dev/GreenMindDB/releases/download/source-2026-10-10/GreenMindDB-source-2026-10-10.tar.gz) | [Cloud setup](https://github.com/Dinten-dev/GreenMindDB/blob/main/README.md#local-development) and [backend guide](https://github.com/Dinten-dev/GreenMindDB/blob/main/backend/README.md) |


## Verify before installation

- Latest Direct: use [the OTA bootstrap guide](direct-upgrade.md) and [Direct SHA256SUMS](https://github.com/Dinten-dev/GreenMindArdu/releases/download/direct-latest/SHA256SUMS).
- Historical v2.8/Gateway sensors: download [SHA256SUMS](https://github.com/Dinten-dev/GreenMindArdu/releases/download/manual-2026-10-10/SHA256SUMS) and verify the ZIP, then each binary against its package manifest.
- Raspberry Pi: download [SHA256SUMS](https://github.com/Dinten-dev/GreenMindRPIv1/releases/download/manual-2026-10-10/SHA256SUMS); the maintenance ZIP contains its own manifest and file checksums.
- Cloud: download [SHA256SUMS](https://github.com/Dinten-dev/GreenMindDB/releases/download/source-2026-10-10/SHA256SUMS) and [source manifest](https://github.com/Dinten-dev/GreenMindDB/releases/download/source-2026-10-10/manifest.json). Check the exact source revision before deployment planning.
- Complete manifests record the source revision and validation limitations. Keep each package's binaries and flash addresses together.

## Choose the correct sensor path

Direct-to-Cloud uses a Direct code from the selected cloud dashboard and requires no Pi.
Direct-to-Gateway uses a Gateway sensor code, a sensor-local web form and a registered Pi.
Both sensor forms are available through `GreenMind-Sensor-XXXX` at `http://192.168.4.1`.
Gateway registration uses its own separate dashboard code. These codes are not interchangeable.

For a Staging test only, use the separate [Direct Staging v2.8 ZIP](https://github.com/Dinten-dev/GreenMindArdu/releases/download/manual-2026-10-10/direct-staging-2.8.zip),
the Staging dashboard, and that package's manifest. Never provision Production credentials into Staging.

## Source repositories

- [Sensor firmware](https://github.com/Dinten-dev/GreenMindArdu)
- [Raspberry Pi Gateway](https://github.com/Dinten-dev/GreenMindRPIv1)
- [Cloud API and dashboard](https://github.com/Dinten-dev/GreenMindDB)

The Pi [release page](https://github.com/Dinten-dev/GreenMindRPIv1/releases/tag/manual-2026-10-10)
also offers the reviewed full source archive. Its general installer enables the update agent;
it is not the maintenance procedure for an existing field Gateway.
Older field releases remain historical references. A newer source branch does not replace a checked release binary.

## Maintaining the latest Direct download

Direct-only releases keep immutable version assets and the dedicated `direct-latest` alias.
For each version, test the exact Production OTA binary on a physical sensor first.
`tools/package_direct_ota_release.py` requires the matching application/bootloader
hashes, source fingerprints and complete physical report before packaging.
Publish immutable version assets, then update `direct-latest` with those same bytes.
The tag workflow validates software candidates; it cannot substitute a rebuilt
binary or update the public alias without the hardware acceptance step.
CI builds do not imply per-board hardware or plant-signal qualification.
