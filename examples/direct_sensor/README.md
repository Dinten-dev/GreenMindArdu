Download the [latest Production Direct ZIP](https://github.com/Dinten-dev/GreenMindArdu/releases/download/direct-latest/direct-to-cloud-production.zip)
and follow the [application-only USB guide](../../docs/direct-upgrade.md).

# Direct firmware and explicit DUAL development

## v2.9-rc1 PSRAM store-and-forward candidate

The explicit `direct_biolingo_v22_psram_test` and
`direct_biolingo_v22_psram_production` targets select the existing Biolingo v22
N16R8 board definition (16 MB flash, octal PSRAM). Verify the actual module
marking and detected PSRAM before flashing. The Production application passed one-device hardware acceptance; it remains
a release candidate and is not qualified for every board or plant measurement. No automatic flashing or OTA is provided.

Acquisition stores compact PCM records in a bounded SPSC byte ring. Each record
remains in the ring while uploading and retrying; it is released only after the
existing device/session/sequence/SHA-256 acknowledgement validation succeeds.
One sensor task owns writes and one uploader owns reads. No dynamic allocation,
network wait, extra ADC reading or filter was added to acquisition. Gateway/DUAL
delivery remains independent and retains its existing separate queue.

At startup the allocator preserves at least 1 MiB free PSRAM and chooses a
power-of-two ring up to 4 MiB, respecting the largest free block. With a 4 MiB
allocation, full 380 Hz mono PCM16 blocks have 5,349 seconds nominal capacity
(about 89 minutes). Partial blocks and other formats change that duration.
No PSRAM means an explicitly labelled 32 KiB RAM fallback (41 nominal seconds)
if internal heap headroom permits; otherwise recording does not start and an
allocation error is shown. A fallback is not accepted for the 120-second field
test. RAM/PSRAM contents do not survive reset or power loss.

The display alternates upload diagnostics with buffer fill and nominal maximum
seconds. Serial output exposes buffer bytes, capacity, PSRAM selection, upload
failures and acquisition/transport losses without credentials. A full ring
rejects new blocks, preserving older unacknowledged data and incrementing the
existing lost-frame counter. No finite buffer promises unlimited outage survival.

Host tests cover a 120-second simulated upload outage spanning a ten-minute
boundary, 1,400 seconds of contiguous samples, repeated/wrong acknowledgements,
full-buffer ownership, 100,000 concurrent records, mixed PCM24 records and
uint32 counter rollover after more than 4 GiB of transport. Other devices still require: actual PSRAM detection, 120 seconds Wi-Fi/server interruption,
zero new lost samples, complete verified cloud replay across multiple segment
boundaries, and stable heap during TLS recovery. Keep the previous application
and NVS pairing data for manual rollback.

Sampling remains 380 Hz with the existing nominal ADC-output calibration.
The HPK comparison uses a different stored stream (400 Hz acquisition, onboard
low-pass/notch filtering, four-sample averaging to 100 Hz and mV-times-ten PCM).
Noise conclusions require original aligned recordings and the actual measurement
setup; the screenshot alone does not identify a hardware or ADC cause.

This folder is a separate PlatformIO project for new, explicitly provisioned
Biolingo v22 ESP32-S3 sensors. The Gateway sensor is built separately in `GreenMindFirmware_Biolingo`; neither target updates an installed sensor automatically.

Build with `platformio run --project-dir examples/direct_sensor`. This creates
an image only; it does not flash or distribute firmware. The example uses a
separate large-app partition layout and has no OTA integration. Do not install
it through the old OTA path or flash an existing production device implicitly.

## Hotspot setup (selected environment)

An unconfigured sensor starts `GreenMind-Sensor-XXXX` and displays the network
name and `192.168.4.1` on its SSD1306 OLED (SDA GPIO13, SCL GPIO12, address 0x3C).
Connect a phone to the hotspot, open the portal, and enter a 2.4-GHz Wi-Fi name,
Wi-Fi password, and the six-character **Direct** pairing code from the selected dashboard.
This requires the new Direct dashboard/backend routes to be deployed and enabled;
legacy Gateway codes do not become Direct credentials just because their length
matches. Previously issued eight-character Direct codes remain accepted.
Hold BOOT for five seconds and release it to reopen setup. Reconfiguration keeps
the device credential and requires a new Direct code for the same organization
and zone. The OLED shows setup/connection state; `TEST CLOUD OK` / `LIVE CLOUD OK` appears only
after a verified upload acknowledgement. Neither the display nor serial logs
show Wi-Fi passwords, pairing codes, or device tokens. No PC/USB monitor is
required to start the display or hotspot.

## Explicit Production target

`platformio run --project-dir examples/direct_sensor -e direct_biolingo_production`
compiles a separate Production image for `https://green-mind.ch`. The default
`direct_biolingo_test` target remains `https://test.green-mind.ch`. CI compiles both.
The portal and OLED show the selected server. Production uses its own NVS
namespace; changing builds cannot reuse a Staging token, device ID or Wi-Fi setup.
Pair again with a **Direct** code from the selected dashboard after the server's
separate Direct activation. Returning to Staging retains its previous pairing.
Neither compilation nor a develop push flashes or upgrades devices automatically.
Serial provisioning also rejects an endpoint belonging to the other environment.

For DUAL, install a reviewed GreenMindRPIv1 release commit supporting protocol
v3 on the participating Pi first. Its `/api/v1/health` must advertise
`ingest_protocol_versions: [1, 2, 3]` and `sequence_acknowledgement: true`.
The hotspot pairs DIRECT mode; DUAL remains explicit operator provisioning with
the matching registered Legacy sensor/zone, a DUAL server identity and local
Gateway URL. There is no automatic sensor migration or cross-path deduplication
in shared analytics; the archives remain separate.

## Operator USB configuration

Configuration is also accepted over local USB serial as one JSON line. Secrets go
into a separate NVS namespace (`gmdirect` for Staging, `gmdirectprod` for Production); they are never printed or compiled
into source. NVS encryption and hardware secure-element provisioning are outside
this test build. The operator must protect physical provisioning access.

| Key | Requirement |
|---|---|
| `transport` | `GATEWAY`, `DIRECT` or `DUAL`; missing means `GATEWAY` |
| `ssid`, `password` | Test Wi-Fi credentials |
| `endpoint` | Exact HTTPS `/api/v1/direct-ingest/chunks` URL for the compiled target |
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
Never put Staging tokens into a Production build or vice versa.

## Acquisition and buffering

The supplied acquisition task reads Biolingo AD8232 on GPIO4 at nominal 380 Hz,
with explicit detection of missed deadlines. It rounds to tenths of millivolts
before both transports, then applies the exact existing Gateway PCM16 mapping.
Direct uses unfiltered nominal ADC output; it does not reproduce the Gateway sensor filters. Hotspot setup and OLED status are implemented
separately for this test build.

Acquisition owns the sample buffer. Separate queues copy blocks by value, so
network retries cannot read an overwritten rotating acquisition buffer. Each
path has its own consumer; Direct outage does not block Gateway transmission.
Direct-only acquisition starts after Wi-Fi and UTC are ready, so startup waits
do not consume its finite measurement buffer. Gateway/DUAL acquisition and
Gateway uploads remain independent of UTC discovery.

The queue boundary is `direct_transport/DirectCore.h`. It also represents signed
24-bit interleaved data without truncation, suitable for a future ADS131M04
acquisition task. The example does **not** implement an ADS131M04 hardware driver.
DUAL is restricted to the common mono/380-Hz/PCM16 profile, also enforced server-side.

Direct now retains compact blocks in the PSRAM spool described above.
The Gateway queue remains four plus one (about five seconds).
The Direct worker reuses its certificate-verified HTTPS connection after reading
and validating each acknowledgement. It closes failed/invalid responses before
retrying the same pending block; credentials and metadata headers are refreshed
for each request. Full queues drop new blocks and
increment path-specific counters; first-frame indices and sequence gaps expose
the loss. Acquisition continues. Retries retain an unacknowledged pending block,
with bounded backoff. Neither acknowledged nor unacknowledged RAM survives power
loss. A durable SD-backed queue is a future implementation, not a delivered claim.

Monitor `direct_dropped`, `gateway_dropped` and `timing_dropped` over USB, plus
upload `elapsed_ms`, connection `reused`, and available `heap`. Any
nonzero growth during the baseline run requires investigation before rollout.
Real ADC timing, offline recovery, heap headroom during TLS, and sensor operation
must still be checked with the first Staging devices.

## Tests

`tests/direct_transport/test_core.cpp` validates default/mode selection, signed
24-bit boundaries, PCM16 conversion, queue overflow/ownership and 10,000 blocks
with a concurrent producer and consumer. Run a host C++17 compiler with pthreads.
An optional output filename writes all 33,001 in-range deci-millivolt encodings
for the backend's comparison against the unchanged Gateway WAV writer.

Publication uses an explicit version tag; building and test success alone do not flash devices.
## OLED status in the manual v2.8 release

The header identifies TEST/LIVE and CLOUD/GW. `OK` requires a validated upload
acknowledgement less than 3.5 seconds old; its activity mark changes with the
acknowledged packet count. `LANGSAM` means 3.5–10 seconds without confirmation,
`KEIN OK` means at least ten seconds. Wi-Fi loss, time synchronization and failed
uploads have explicit states; a successful HTTP code with an invalid acknowledgement
is still a failure. There is no claim that Gateway acknowledgement proves Cloud arrival.

On the two-colour SSD1306, the heading stays within the yellow top 16 pixels;
all five status rows start in the blue area and fit within 64 pixels.
Five rows show sensor MAC suffix and Wi-Fi RSSI; last-second mean/min/max in mV;
confirmation interval and acknowledgement age (`Takt 1.0s OK 0s`); confirmed
packet count and request duration (or failed HTTP/client status); and lost
samples plus failed attempts since boot. Stale voltage displays `U --mV -- --`.
The voltage observes the same unfiltered samples and nominal 0–3300 mV conversion
as the uploaded recording; it is amplifier output, not calibrated electrode voltage.
No extra ADC read or smoothing is introduced. The one-second upload target and
380 Hz sampling rate remain unchanged. The measured interval is smoothed over
recent acknowledgements and hidden when stale or when fewer than two have arrived.
The final row abbreviates failed attempts as `E`. Counts use k/M/G suffixes for long runs. Failures can recover through retry; they
are not automatically lost samples. Loss combines the selected path and sampling
timing losses. DUAL rotates independent CLOUD/GW pages every six seconds, marked D.
Settings and BOOT-to-setup behaviour are retained. Only the main loop draws to
the OLED; acquisition and both upload tasks remain independent.

## Manual v2.8 OLED update

The OLED shows sensor MAC suffix and mean/minimum/maximum of the most recent completed 380-sample second, in mV. It observes the existing ADC values; calibration, sampling, filtering, pairing, data payload, retry/spool, endpoint and ACK validation remain unchanged. Stale readings display `--`. No OTA distribution is provided.
