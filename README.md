# GreenMind ESP32 Sensor Firmware

Field candidate, corrected USB flash layout, and the three firmware choices:
[25 September 2026 greenhouse checklist](docs/GREENHOUSE-2026-09-25.md).

🎥 **Video Presentation:** [Watch our showcase at the Science Exhibition](https://youtu.be/OdKqk1Vc4Uo?si=9BqRJsJWqswzQC3o) — A short introduction to what we have discovered so far.
> C++ firmware (PlatformIO/Arduino) for GreenMind bioelectric plant sensors on ESP32-S3. Captures bioelectrical signals at **380 Hz** with a two-stage digital filter (20 Hz EMA lowpass + 50 Hz biquad notch), sends data to the Raspberry Pi Gateway via HTTP POST, provisions Wi-Fi over BLE, and supports OTA firmware updates.

> **⚠️ R&D Status:** Part of the [GreenMind](https://github.com/Dinten-dev/GreenMindDB) research platform by **Galaxyadvisors AG** in collaboration with FHNW.

---

## Table of Contents

1. [Signal Acquisition](#signal-acquisition)
2. [Firmware Variants](#firmware-variants)
3. [Hardware Setup](#hardware-setup)
4. [Project Structure](#project-structure)
5. [Prerequisites](#prerequisites)
6. [Flashing](#flashing)
7. [Provisioning and Pairing](#provisioning-and-pairing)
8. [Gateway Discovery](#gateway-discovery)
9. [OTA Updates](#ota-updates)
10. [Reset Behavior](#reset-behavior)
11. [Artifact Detection](#artifact-detection-ad8232--biolingo)
12. [Security and Verification](#security-and-verification)
13. [Related Repositories](#related-repositories)
14. [Author & Credits](#author--credits)
15. [License](#license)

---

## Signal Acquisition

| Parameter | Value |
|-----------|-------|
| **Sample Rate** | 380 Hz (timer-based, ~2632 µs interval) |
| **ADC Resolution** | 12-bit (0–4095) |
| **ADC Range** | 0–3.3 V |
| **Output Unit** | Millivolt (mV) |
| **Filter** | 20 Hz EMA Lowpass + 50 Hz Biquad Notch (Q=30) |
| **Batch Size** | 380 samples per HTTP POST (= 1 second) |
| **Amplifier** | AD8232 bioelectric signal amplifier |

### Data Flow

```
Plant → AD8232 → GPIO (ADC) → 380 Hz timer → EMA LP → Notch 50Hz → mV
  → Buffer (380 samples) → HTTP POST to Gateway /api/v1/ingest
```

### JSON Payload (per POST)

```json
{
  "mac_address": "AA:BB:CC:DD:EE:FF",
  "sample_rate": 380,
  "readings": [
    {"kind": "bio_signal", "value": 1523.4, "unit": "mV"},
    {"kind": "bio_signal", "value": 1518.7, "unit": "mV"}
  ]
}
```

---

## Firmware

| Directory | Board | Framework | Features |
|-----------|-------|-----------|----------|
| `GreenMindFirmware_Biolingo/` | ESP32-S3 (Biolingo v22) | PlatformIO | OTA updates, OLED display, BLE Wi-Fi provisioning, 380 Hz streaming, AD8232 artifact detection |

> 📦 Archived ESP32-WROOM variants are retained in `archive/`. The root `GreenMind/` directory is a clearly marked legacy MicroPython prototype and is not part of the production build.

---

## Hardware Setup

### Biolingo v22 Custom PCB (ESP32-S3)

| Component | Pin | ESP32-S3 GPIO |
|-----------|-----|---------------|
| **AD8232 OUTPUT** | ADC1_CH3 | `IO4` |
| **AD8232 LOD+** | Lead-Off + | `IO5` |
| **AD8232 LOD-** | Lead-Off - | `IO6` |
| **SSD1306 SCL** | I2C Clock | `IO12` |
| **SSD1306 SDA** | I2C Data | `IO13` |
| **Boot Button** | Active Low | `IO0` |

### ⚡ Stromversorgung (WICHTIG)

> **Jeder Sensor MUSS über ein eigenes USB-Netzteil versorgt werden — NICHT am Raspberry Pi USB!**

Die gemeinsame Masse über den USB-Port des RPi erzeugt eine **50-Hz-Masseschleife**, die den AD8232-Verstärker in Sättigung treibt. Das Signal wird dadurch unbrauchbar.

```
✅ RICHTIG                          ❌ FALSCH

Steckdose ─── Netzteil A ─── RPi    Steckdose ─── Netzteil ─── RPi
Steckdose ─── Netzteil B ─── ESP32              └── USB ──── ESP32
                                                    ↑ 50 Hz Ground Loop!
```

- **Sensor-Netzteil:** USB 5V / 500 mA (beliebig)
- **RPi-Netzteil:** Offizielles Raspberry Pi Netzteil (5V / 3A)

> Die aktive Firmware enthält einen digitalen 50-Hz-Notchfilter als Absicherung, aber die **physische Trennung bleibt zwingend** für saubere Signale.

---

## Project Structure

```
GreenMindArdu/
├── flash-sensor.sh                 # Local PlatformIO build/flash wrapper
├── GreenMindFirmware_Biolingo/     # Active firmware (ESP32-S3, PlatformIO)
│   ├── platformio.ini              # ESP32-S3 build config
│   ├── partitions.csv              # Custom partition table (OTA)
│   └── src/
│       ├── main.cpp                # Main firmware logic
│       ├── display.cpp / .h        # SSD1306 OLED display driver
│       └── ota_client.cpp / .h     # OTA update client
├── archive/                        # Archived firmware variants
│   ├── GreenMindFirmware/          # ESP32-WROOM production (Arduino IDE)
│   ├── GreenMindFirmware_AD8232/   # ESP32-WROOM biosignal R&D
│   └── GreenMindFirmware_OTA/      # ESP32-WROOM OTA-enabled
├── GreenMind/                      # MicroPython prototype (legacy)
├── .gitignore
├── LICENSE
└── README.md
```

---

## Prerequisites

- **PlatformIO CLI** or **PlatformIO IDE** (VS Code extension)
- Platform and library versions are pinned in `platformio.ini` for reproducible builds
- **PlatformIO Core 6.1.19** (the version used by CI)

---

## Flashing

The active target is **ESP32-S3-WROOM-1-N16R8**. The first v1.1.0 installation
must use USB because it replaces the flash partition table. Later application
updates can use OTA normally.

### Reviewed local flash

Clone and inspect the repository, create an isolated tool environment, then plug in the ESP32-S3 and flash it:

```bash
git clone https://github.com/Dinten-dev/GreenMindArdu.git
cd GreenMindArdu
python3 -m venv .venv
source .venv/bin/activate
python -m pip install platformio==6.1.19
./flash-sensor.sh
```

Use `./flash-sensor.sh --port /dev/your-device --monitor` when automatic serial-port detection is ambiguous. The wrapper never downloads tools or executes remote installers. Archived variants are reference material and are not exposed as production choices.

### Manual Flash (Alternative)

#### PlatformIO (recommended)

```bash
cd GreenMindFirmware_Biolingo
pio run --environment biolingo_v22 --target upload
pio device monitor
```



---

## Provisioning and Pairing

The firmware contains no build-time Wi-Fi credentials. A device without valid NVS credentials uses the runtime provisioning flow:

1. The sensor advertises over BLE as `GM-XXXX`, where `XXXX` is the final four hexadecimal characters of its MAC address.
2. The OLED displays a six-character proof-of-possession code generated from the ESP32 hardware random-number source; serial logs do not expose it.
3. An Espressif-compatible Security 1 provisioning client supplies the Wi-Fi SSID and password using that BLE name and proof-of-possession.
4. The firmware stores the provisioned network in NVS, restarts, discovers the gateway, and begins streaming.

Cloud sensor association is a separate existing flow. If a dashboard pairing code is present in NVS, the firmware submits it once to `POST /api/v1/sensors/register` through the gateway and then clears it. The current BLE Wi-Fi provisioning callback does not itself populate that dashboard pairing code; gateway/dashboard automation must account for this distinction.

---

## Gateway Discovery

The firmware uses a three-tier discovery strategy:

1. **Cached IP** — try previously known gateway address (stored in NVS)
2. **UDP Broadcast** — send `DISCOVER_GREENMIND_GATEWAY` on port 50000
3. **Subnet Scan** — fall-back full scan of the local `/24` network

The discovered IP is cached in NVS for subsequent boots.

---

## OTA Updates

The firmware supports over-the-air updates via the Raspberry Pi Gateway:

1. The sensor checks the gateway's `/api/v1/ota/check` endpoint at boot and every hour.
2. It validates that the supplied SHA-256 is a 64-character hexadecimal digest.
3. It rejects missing, short, oversized, timed-out, or hash-mismatched downloads and aborts the inactive OTA partition.
4. Only a length- and SHA-256-verified image is finalized as bootable; the next boot is reported to the gateway.

The custom `partitions.csv` allocates dual OTA slots and a durable LittleFS spool:
```
# Name,   Type, SubType, Offset,    Size
nvs,      data, nvs,     0x009000,  0x006000
otadata,  data, ota,     0x00F000,  0x002000
app0,     app,  ota_0,   0x020000,  0x300000
app1,     app,  ota_1,   0x320000,  0x300000
spiffs,   data, spiffs,  0x620000,  0x9D0000
coredump, data, coredump,0xFF0000,  0x010000
```

Failed one-second batches use CRC-protected binary spool segments. Uploads are
replayed oldest-first and erased only after the gateway confirms the exact
`boot_id` and `sequence`. At 380 Hz, the spool holds roughly three hours.

Sampling uses a dedicated hardware timer and high-priority task. Wi-Fi,
discovery, display, OTA, and HTTP work cannot intentionally block the sampler;
any missed timer notifications remain explicit data-quality gaps.

---

## Reset Behavior

The active firmware does not expose an inbound HTTP reset endpoint. Deleting a cloud sensor therefore does not erase device NVS. Reset or reprovision a device through an authorized local flashing/erase workflow until a coordinated authenticated control protocol is implemented across sensor, gateway, and backend.

---

## Artifact Detection (AD8232 / Biolingo)

The AD8232-based variants perform real-time signal quality assessment:

| Flag | Bit | Condition |
|------|-----|-----------|
| `VALID` | 0 | Clean signal |
| `LEAD_OFF` | 1 | AD8232 LOD+ or LOD- high (electrodes disconnected) |
| `RAIL_HIGH` | 2 | Signal > 3200 mV (ADC saturation) |
| `RAIL_LOW` | 4 | Signal < 100 mV (ADC floor) |
| `JUMP` | 8 | |Δ| > 500 mV between consecutive samples |
| `RECOVERY` | 16 | 100 ms cooldown window after any artifact |

Flags are retained in the firmware's internal batch while sampling. The current production ingest payload intentionally remains `{kind, value, unit}` for compatibility and does **not** transmit these flags; adding them requires a coordinated gateway/backend schema change.

---

## Security and Verification

- Never add Wi-Fi credentials or other secrets to `platformio.ini`; provisioning data belongs in device NVS.
- OTA images are checked against the SHA-256 metadata supplied by the gateway before the partition is finalized.
- SHA-256 protects the image only if the metadata source is trusted. Sensor-to-gateway OTA still uses local HTTP and has no independent firmware signature, so a compromised or impersonated gateway remains an OTA trust risk.
- Batch buffers come from a fixed ownership pool. When uploads cannot keep up, the firmware logs cumulative dropped samples instead of reusing in-flight memory.

Run the focused source-invariant checks and the firmware build with:

```bash
python3 -m unittest discover -s tests -v
cd GreenMindFirmware_Biolingo
pio run
```

The firmware CI workflow runs those checks with Python 3.12.8, PlatformIO 6.1.19, and
clang-format 18.1.8. Formatting is defined by the root `.clang-format` and applies only to
`GreenMindFirmware_Biolingo/src` and `GreenMindFirmware_Biolingo/include` when present;
archived variants are intentionally excluded.

---

## Related Repositories

| Repository | Description |
|---|---|
| **[GreenMindDB](https://github.com/Dinten-dev/GreenMindDB)** | Cloud backend (FastAPI), frontend (Next.js), Docker infrastructure |
| **[GreenMindRPI](https://github.com/Dinten-dev/GreenMindRPIv1)** | Raspberry Pi gateway — data aggregation, WAV recording, OTA agent |
| **GreenMindArdu** *(this repo)* | ESP32 sensor firmware |

---

## Author & Credits

**Traver Dinten** — [Galaxyadvisors AG](https://galaxyadvisors.com), Aarau, Switzerland

Developed in collaboration with **FHNW** (Fachhochschule Nordwestschweiz) as part of the KI-Programmierung module.

---

## License

This project is licensed under the MIT License — see the [LICENSE](LICENSE) file for details.
