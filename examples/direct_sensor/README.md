# Explicit Direct / DUAL sensor test build

This folder is a separate PlatformIO project for new, explicitly provisioned
Biolingo v22 ESP32-S3 test sensors. `GreenMindFirmware_Biolingo` and its existing
configuration, partition table and firmware behaviour remain untouched.

Build with `platformio run --project-dir examples/direct_sensor`. This creates
an image only; it does not flash or distribute firmware. The example uses a
separate large-app partition layout and has no OTA integration. Do not install
it through the old OTA path or flash an existing production device implicitly.

## Hotspot setup on Staging

An unconfigured sensor starts `GreenMind-Sensor-XXXX` and displays the network
name and `192.168.4.1` on its SSD1306 OLED (SDA GPIO13, SCL GPIO12, address 0x3C).
Connect a phone to the hotspot, open the portal, and enter a 2.4-GHz Wi-Fi name,
Wi-Fi password, and the six-character **Direct** pairing code from Staging.
This requires the new Direct dashboard/backend routes to be deployed and enabled;
legacy Gateway codes do not become Direct credentials just because their length
matches. Previously issued eight-character Direct codes remain accepted.
Hold BOOT for five seconds and release it to reopen setup. Reconfiguration keeps
the device credential and requires a new Direct code for the same organization
and zone. The OLED shows setup/connection state; `Cloud: Daten OK` appears only
after a verified upload acknowledgement. Neither the display nor serial logs
show Wi-Fi passwords, pairing codes, or device tokens. No PC/USB monitor is
required to start the display or hotspot.

## Operator USB configuration

Configuration is also accepted over local USB serial as one JSON line. Secrets go
into a separate NVS namespace `gmdirect`; they are never printed or compiled
into source. NVS encryption and hardware secure-element provisioning are outside
this test build. The operator must protect physical provisioning access.

| Key | Requirement |
|---|---|
| `transport` | `GATEWAY`, `DIRECT` or `DUAL`; missing means `GATEWAY` |
| `ssid`, `password` | Test Wi-Fi credentials |
| `endpoint` | Full reviewed Staging HTTPS `/api/v1/direct-ingest/chunks` URL |
| `device_id`, `token` | Device identity/key issued by the Direct server |
| `ca` | PEM CA certificate chain trusted for the HTTPS endpoint |
| `gateway` | Local test Gateway base URL, required for GATEWAY and DUAL |

Only DIRECT requires no Gateway. DUAL/GATEWAY test sensors must be registered
with the existing test Gateway before streaming. The example does not implement
Gateway discovery or automatically claim a sensor. Incorrect provisioning is
reported without printing the configuration. Sending a new valid configuration
restarts the test device and creates a new session.

The Gateway target is local HTTP because that is the existing Gateway protocol.
The new cloud path always verifies HTTPS certificates and refuses redirects.
Do not put Gateway or production cloud credentials into this Direct test setup.

## Acquisition and buffering

The supplied acquisition task reads Biolingo AD8232 on GPIO4 at nominal 380 Hz,
with explicit detection of missed deadlines. It rounds to tenths of millivolts
before both transports, then applies the exact existing Gateway PCM16 mapping.
The new build is for transport comparison, not a replacement for the existing
firmware's filters or OTA behaviour. Hotspot setup and OLED status are implemented
separately for this test build.

Acquisition owns the sample buffer. Separate queues copy blocks by value, so
network retries cannot read an overwritten rotating acquisition buffer. Each
path has its own consumer; Direct outage does not block Gateway transmission.
UTC discovery does not block acquisition or Gateway uploads. Direct retains
pending RAM blocks until UTC becomes available.

The queue boundary is `direct_transport/DirectCore.h`. It also represents signed
24-bit interleaved data without truncation, suitable for a future ADS131M04
acquisition task. The example does **not** implement an ADS131M04 hardware driver.
DUAL is restricted to the common mono/380-Hz/PCM16 profile, also enforced server-side.

Queues hold four waiting blocks plus one pending upload per path. At normal
one-second blocks this is about five seconds. Full queues drop new blocks and
increment path-specific counters; first-frame indices and sequence gaps expose
the loss. Acquisition continues. Retries retain an unacknowledged pending block,
with bounded backoff. Neither acknowledged nor unacknowledged RAM survives power
loss. A durable SD-backed queue is a future implementation, not a delivered claim.

Monitor `direct_dropped`, `gateway_dropped` and `timing_dropped` over USB. Any
nonzero growth during the baseline run requires investigation before rollout.
Real ADC timing, offline recovery, heap headroom during TLS, and sensor operation
must still be checked with the first Staging devices.

## Tests

`tests/direct_transport/test_core.cpp` validates default/mode selection, signed
24-bit boundaries, PCM16 conversion, queue overflow/ownership and 10,000 blocks
with a concurrent producer and consumer. Run a host C++17 compiler with pthreads.
An optional output filename writes all 33,001 in-range deci-millivolt encodings
for the backend's comparison against the unchanged Gateway WAV writer.

No production push or flashing is authorized by building or passing these tests.
# Staging CI follow-up, 12 September 2026

The existing firmware CI also failed on the published main baseline because
`GreenMindFirmware_Biolingo/src/main.cpp` and `src/sensor_spool.cpp` did not match
the repository's pinned clang-format 18.1.8 rules. Only formatting in those two
existing files was corrected for develop. Their C++ token sequences were checked
to be identical, and the existing security tests and firmware build were rerun.
No sensor was flashed and no existing firmware logic was changed.
