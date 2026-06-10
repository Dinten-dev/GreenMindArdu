# GreenMind ESP32 Sensor Firmware

> C++ firmware (PlatformIO/Arduino) for GreenMind bioelectric plant sensors on ESP32-S3. Captures bioelectrical signals at **380 Hz** with a 3-sample Moving Average filter, sends data to the Raspberry Pi Gateway via HTTP POST. Supports automatic provisioning via captive portal and OTA firmware updates.

> **⚠️ R&D Status:** Part of the [GreenMind](https://github.com/Dinten-dev/GreenMindDB) research platform by **Galaxyadvisors AG** in collaboration with FHNW.

---

## Table of Contents

1. [Signal Acquisition](#signal-acquisition)
2. [Firmware Variants](#firmware-variants)
3. [Hardware Setup](#hardware-setup)
4. [Project Structure](#project-structure)
5. [Prerequisites](#prerequisites)
6. [Flashing](#flashing)
7. [Pairing Workflow](#pairing-workflow)
8. [Gateway Discovery](#gateway-discovery)
9. [OTA Updates](#ota-updates)
10. [Remote Reset](#remote-reset)
11. [Related Repositories](#related-repositories)
12. [Author & Credits](#author--credits)
13. [License](#license)

---

## Signal Acquisition

| Parameter | Value |
|-----------|-------|
| **Sample Rate** | 380 Hz (timer-based, ~2632 µs interval) |
| **ADC Resolution** | 12-bit (0–4095) |
| **ADC Range** | 0–3.3 V |
| **Output Unit** | Millivolt (mV) |
| **Filter** | 3-sample Moving Average |
| **Batch Size** | 380 samples per HTTP POST (= 1 second) |
| **Amplifier** | AD8232 bioelectric signal amplifier |

### Data Flow

```
Plant → AD8232 → GPIO (ADC) → 380 Hz timer → Moving Avg → mV conversion
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
| `GreenMindFirmware_Biolingo/` | ESP32-S3 (Biolingo v22) | PlatformIO | OTA updates, OLED display, captive portal, 380 Hz streaming, AD8232 artifact detection |

> 📦 Archived ESP32-WROOM variants (GreenMindFirmware, AD8232, OTA) and a MicroPython prototype are available in `archive/`.

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

---

## Project Structure

```
GreenMindArdu/
├── flash-sensor.sh                 # 🚀 One-liner flash tool (curl-pipe-bash)
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
- Dependencies are auto-resolved from `platformio.ini`
- The [flash-sensor.sh](flash-sensor.sh) script installs PlatformIO automatically if missing

---

## Flashing

### One-Liner Flash

Plug in your ESP32-S3 (Biolingo v22) via USB-C and run:

```bash
curl -fsSL https://raw.githubusercontent.com/Dinten-dev/GreenMindArdu/main/flash-sensor.sh | bash
```

This single command handles everything:

| Step | Action | Details |
|------|--------|---------|
| **1** | Source Code | Clones the firmware repository (or uses local copy) |
| **2** | Toolchain | Installs PlatformIO with ESP32-S3 support |
| **3** | USB Detection | Auto-detects the ESP32-S3 serial port |
| **4** | Firmware | Selects GreenMindFirmware_Biolingo (ESP32-S3 Biolingo v22) |
| **5** | Compile & Flash | Builds and uploads the firmware in one step |
| **6** | Verification | Optionally opens serial monitor to verify boot |

> **Note:** The script is safe to re-run — it skips tools that are already installed and only downloads what's needed.

> ⚠️ Make sure you're using a **data-capable USB cable** (not charge-only). If no device is detected, the script provides driver installation instructions.

> 📦 Archived ESP32-WROOM firmware variants (GreenMindFirmware, AD8232, OTA) are available in the `archive/` directory.

### Manual Flash (Alternative)

#### PlatformIO (recommended)

```bash
cd GreenMindFirmware_Biolingo
pio run --target upload
pio device monitor
```



---

## Pairing Workflow

1. Flash the firmware and boot the ESP32
2. The sensor detects no WiFi credentials in NVS
3. **Setup Mode**: Creates an Access Point named `GreenMind-Sensor-XXXX` (last 4 hex chars of MAC)
4. Connect your phone to the AP — a captive portal opens automatically
5. Enter:
   - **WiFi SSID** — your local network
   - **WiFi Password**
   - **Pairing Code** — 6-character code from the GreenMind Dashboard
6. The ESP32 saves credentials to NVS, reboots, and connects to WiFi
7. It discovers the Gateway via UDP broadcast, registers with the pairing code, and starts streaming at 380 Hz

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

1. The sensor checks the gateway's `/api/v1/firmware/check` endpoint periodically (every 1 hour)
2. If a newer firmware is available, the binary is downloaded and verified via **SHA256**
3. The update is applied via the ESP32 OTA partition scheme
4. On failure, the device rolls back to the previous firmware automatically

The custom `partitions.csv` allocates space for two OTA slots:
```
# Name,   Type, SubType, Offset,  Size
nvs,      data, nvs,     0x9000,  0x5000
otadata,  data, ota,     0xe000,  0x2000
app0,     app,  ota_0,   0x10000, 0x1E0000
app1,     app,  ota_1,   0x1F0000,0x1E0000
spiffs,   data, spiffs,  0x3D0000,0x30000
```

---

## Remote Reset

If you delete the sensor via the Dashboard, the Gateway transmits an HTTP `DELETE` command. The ESP32 immediately wipes its NVS config and reboots into Setup Mode with a fresh captive portal.

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

Flags are transmitted per-sample as a bitmask in the `flags` field.

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
