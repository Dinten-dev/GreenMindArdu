# Direct durable spool candidate: N16R8

The explicit `direct_biolingo_n16_flash_test` and
`direct_biolingo_n16_flash_production` targets add a persistent Direct journal.
Existing generic, N8R8, Gateway, Dual and OTA targets keep their current behavior.
This is a USB-only protocol-1 candidate, not an OTA release. Field sensors have
not received this firmware. No N8R8 hardware was available; the 16 MiB layout
must never be installed on an 8 MiB sensor.

## Ownership and durability

The N16 layout preserves the original NVS, application, legacy filesystem and
coredump offsets. A new `directspool` partition occupies 0x400000..0xFFFFFF.
Formatting is permitted only when mounting fails and every byte of that new
partition is erased (0xFF). Unknown or damaged existing content is retained and
acquisition stays paused. A device/endpoint ownership file prevents accidental
cross-device or Staging/Production replay.

The existing PSRAM queue feeds a separate flash writer. Each record contains
sequence, first frame, frame count, PCM and a CRC. A frozen metadata envelope
retains the original device, endpoint, session, clock mapping, sample format,
firmware version and signal profile. It contains no token or WLAN settings.
The RAM record is acknowledged only after flush, fsync, close and independent
readback. Network retries do not hold the filesystem mutex.

The uploader consumes flash in FIFO order and checks the server's complete
device/session/sequence/payload-SHA acknowledgement before advancing. Files are
deleted only after all complete records have been acknowledged and the writer
has moved to another file. A reboot may replay already acknowledged records;
the receiver's idempotence handles those duplicates. Uploads after reboot use
their original session metadata, not the current firmware or boot session.

An interrupted append preserves its valid prefix and retains its incomplete
suffix as `.incomplete`. A bad CRC stops processing without formatting or
overwriting the evidence. The bounded journal never overwrites unacknowledged
records. When full, the writer waits and PSRAM temporarily absorbs new samples;
if both fill, new samples can be lost and loss counters increase. Only records
already committed to flash survive abrupt power loss; a partial acquisition
block and records still in RAM are not durable.

The 12 MiB partition provides roughly four hours at nominal 400 Hz mono PCM16
with one-second records, before filesystem overhead and retained evidence.
This is a capacity estimate, not an hours-long outage acceptance. Flash has
finite write endurance. An accurate initial clock synchronization is required
before starting a fresh capture; offline cold-boot acquisition is not added.

## Acquisition during flash writes

The timer-based prototype stalled during flash writes and fragmented captures;
it failed hardware acceptance and must not be released. The durable target now
uses the pinned ESP32-S3 continuous ADC driver at 2000 hardware conversions/s
and retains every fifth real value, unchanged, for nominal 400 Hz PCM16. There
is no averaging, interpolation or digital filtering. Existing non-durable
profiles continue using their existing acquisition path.

Five 1024-byte DMA frames cover about 640 ms, with a 16 KiB driver pool behind
them. A delivery gap over 400 ms, malformed channel/unit data or a driver error
pauses acquisition. Samples and an exact missing-frame count are never
fabricated. A journal fault also pauses acquisition. Serial diagnostics expose
raw conversions, retained samples, delivery gaps, flash backlog, committed
records and failure counters.

Delivery latency and observed sample throughput do not establish ADC jitter,
absolute timestamp accuracy, NTP offset or long-term oscillator drift. Those
scientific timing validations remain separate.

## Hardware evidence, 8 November 2026

One separately identified ESP32-S3 N16R8 testboard was used against
`test.green-mind.ch`; production field boards were not flashed.

- The first DMA candidate produced 72,038 retained values over 180.143 s
  (399.893 Hz observed), with zero reported DMA errors or dropped samples.
- A read-only journal snapshot before a controlled testboard restart contained
  715 committed DMA records. All 715 were later found on Staging with identical
  payload SHA-256, frame positions and original capture metadata.
- Failed timer-prototype records were preserved byte-for-byte as diagnostic
  evidence on the testboard and in a private snapshot; they were not deleted or
  relabeled as successfully uploaded. The server's 1200-chunk guard stayed on.
- The final RC3 build was written and read back without changing NVS, the
  partition table or journal. A further 180.084 s observation retained 72,039
  samples (400.029 Hz observed), with zero reported DMA/flash errors or dropped
  samples. Maximum DMA delivery gap was 141,346 us, below the 400,000 us guard.
  Fresh chunks and a published 400 Hz WAV were verified by SHA-256 against
  Staging storage.

The temporary testboard-only evidence-recovery helper is absent from the final
source and build. Preserved diagnostic files remain within the capacity budget.
No private snapshots, credentials or device metadata are included in this repo.

## Deployment boundary

Before a manual installation, physically identify flash/PSRAM capacity and
inspect the existing partition table. This layout is compatible with the
original single application at 0x10000 and legacy filesystem at 0x310000;
other layouts require a separate migration plan. Never erase NVS, an existing
filesystem or an occupied proposed journal region. The standard upload hook
refuses a physical chip smaller than the configured flash size, but it does
not establish layout compatibility or permission to replace occupied regions.

Do not publish this candidate as a field release or use its N16 result as N8R8
acceptance. The separate N8R8 persistent layout, physical outage/restart test
and field USB rollout remain outstanding. Backend receiver compatibility and
mail monitoring are independently deployed; this change does not deploy a
frontend, gmlive replacement or Claude's separate Staging dashboard.
