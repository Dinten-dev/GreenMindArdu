/**
 * Explicit Direct/DUAL firmware for new/provisioned Biolingo ESP32-S3 devices.
 * OTA is explicit on the N16R8 target; USB bootstrap retains pairing.
 * DIRECT: GPIO4, nominal 400 Hz raw PCM16. GATEWAY/DUAL: avg20ms at 380 Hz.
 * ADS131M04 drivers can submit interleaved SampleBlock PCM24 independently.
 */
#include <Arduino.h>
#include <driver/adc.h>
#include "../../../transport_validation/MainsAverage20ms.h"
#include "../../../direct_transport/Ad8232Raw400.h"
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <mbedtls/sha256.h>
#include <sys/time.h>
#include <esp_sntp.h>
#include <nvs.h>

#include "../../../direct_transport/DirectCore.h"
#include "../../../direct_transport/BootIdentity.h"
#include "../../../direct_transport/CaptureClock.h"
#include "../../../direct_transport/PcmSpool.h"
#include "../../../transport_validation/Acknowledgement.h"
#include "StatusDisplay.h"
#include "UploadStatus.h"

using greenmind::Mode;
using greenmind::SampleBlock;
static Preferences preferences;
static Mode mode = Mode::Gateway;
static String ssid, password, endpoint, token, deviceId, certificate, gateway, pairingCode;
static char sessionId[37];
static uint64_t sessionStartUs = 0;
static greenmind::CaptureMetadata captureMetadata;
static portMUX_TYPE syncLock = portMUX_INITIALIZER_UNLOCKED;
static uint64_t syncEpochUs = 0, syncMonotonicUs = 0;
#ifndef GREENMIND_DIRECT_PROTOCOL_VERSION
#define GREENMIND_DIRECT_PROTOCOL_VERSION 1
#endif
static_assert(GREENMIND_DIRECT_PROTOCOL_VERSION == 1 || GREENMIND_DIRECT_PROTOCOL_VERSION == 2,
              "Unsupported Direct protocol version");

static void onTimeSync(timeval* time) {
    if (!time || time->tv_sec < 1700000000) return;
    const uint64_t monotonic = esp_timer_get_time();
    const uint64_t epoch = uint64_t(time->tv_sec) * 1000000ULL + time->tv_usec;
    portENTER_CRITICAL(&syncLock);
    syncEpochUs = epoch;
    syncMonotonicUs = monotonic;
    portEXIT_CRITICAL(&syncLock);
}
static bool syncSnapshot(uint64_t& epoch, uint64_t& monotonic) {
    portENTER_CRITICAL(&syncLock);
    epoch = syncEpochUs;
    monotonic = syncMonotonicUs;
    portEXIT_CRITICAL(&syncLock);
    return epoch >= 1700000000000000ULL;
}

// One atomic NVS blob commit prevents counter/generation split on power loss.
// This namespace is intentionally independent of provisioning and Wi-Fi setup.
static bool allocateBootIdentity() {
    nvs_handle_t handle;
    if (nvs_open("gmdirecttime", NVS_READWRITE, &handle) != ESP_OK) return false;
    struct NvsStore {
        nvs_handle_t handle;
        greenmind::IdentityRead read(greenmind::BootIdentity& value) {
            size_t length = sizeof(value);
            const esp_err_t error = nvs_get_blob(handle, "identity", &value, &length);
            if (error == ESP_ERR_NVS_NOT_FOUND) return greenmind::IdentityRead::Missing;
            return error == ESP_OK && length == sizeof(value) ?
                greenmind::IdentityRead::Valid : greenmind::IdentityRead::Error;
        }
        bool writeAndCommit(const greenmind::BootIdentity& value) {
            return nvs_set_blob(handle, "identity", &value, sizeof(value)) == ESP_OK &&
                   nvs_commit(handle) == ESP_OK;
        }
    } store{handle};
    greenmind::BootIdentity identity{};
    const bool valid = greenmind::allocateBootIdentity(store,
        [](uint8_t* bytes, size_t length) { esp_fill_random(bytes, length); }, identity);
    nvs_close(handle);
    if (!valid) return false;
    const uint8_t* b = identity.generation;
    snprintf(captureMetadata.counterGeneration, sizeof(captureMetadata.counterGeneration),
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9],
        b[10], b[11], b[12], b[13], b[14], b[15]);
    captureMetadata.bootCounter = identity.counter;
    return true;
}
static std::atomic<bool> hasClock{false};
static uint64_t sequence = 0;
static greenmind::PcmSpool directQueue;
static bool directPsram = false;
static greenmind::SpscQueue<SampleBlock, 4> gatewayQueue;
static SampleBlock acquisitionBlock, directPending, gatewayPending;
static std::atomic<uint32_t> directDropped{0}, gatewayDropped{0}, timingDropped{0};
static bool configured = false, identityConfigured = false;
static std::atomic<uint32_t> wifiGotIpGeneration{0};
static greenmind::UploadStatus directStatus, gatewayStatus;
static String provisionLine;
// Display-only snapshot of the same mains-averaged sample sent to the cloud.
static std::atomic<uint32_t> rawConversions{0}, rawClipped{0}, adcRestarts{0}, filteredFrames{0};
static std::atomic<bool> adcHealthy{false}, adcFault{false};
#include "../../../transport_validation/SignalDisplayStats.h"
#include "DemandLive.h"

static WiFiClientSecure directTransport;
static HTTPClient directHttp;

static void makeSessionId() {
    uint8_t bytes[16];
    esp_fill_random(bytes, sizeof(bytes));
    bytes[6] = (bytes[6] & 0x0f) | 0x40;
    bytes[8] = (bytes[8] & 0x3f) | 0x80;
    snprintf(sessionId, sizeof(sessionId),
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
        bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
}

#include "SetupPortal.h"

static String sha256(const uint8_t* data, size_t length) {
    uint8_t digest[32];
    mbedtls_sha256_ret(data, length, digest, 0);
    char hex[65];
    for (size_t i = 0; i < 32; ++i) snprintf(hex + i * 2, 3, "%02x", digest[i]);
    return String(hex);
}

// ADC implementations call this from their sole acquisition task. Submission
// never waits for Wi-Fi or for either consumer, including in DUAL mode.
static void submit(SampleBlock& block) {
#ifdef GREENMIND_DIRECT_ADC_SELFTEST
    return; // Offline hardware test only: no uploads, pairing or NVS access.
#endif
    if (!block.valid()) { timingDropped.fetch_add(block.frames); return; }
    if (mode == Mode::Dual && (block.channels != 1 || block.sampleBits != 16 || block.sampleRate != 380)) {
        timingDropped.fetch_add(block.frames); return;
    }
    block.capture = captureMetadata;
    block.sequence = sequence++;
    if (mode == Mode::Direct || mode == Mode::Dual) {
        if (!directQueue.push(block)) directDropped.fetch_add(block.frames);
    }
    if (mode == Mode::Gateway || mode == Mode::Dual) {
        if (!gatewayQueue.push(block)) gatewayDropped.fetch_add(block.frames);
    }
}

static bool sendDirect(const SampleBlock& block) {
    // Owned exclusively by directWorker. Retain the TLS socket across uploads;
    // constructing these per block made a handshake slower than acquisition.
    auto& transport = directTransport;
    auto& http = directHttp;
    if (WiFi.status() != WL_CONNECTED || !hasClock.load(std::memory_order_acquire)) return false;
    const String digest = sha256(block.payload, block.byteCount());
    JsonDocument metadata;
    metadata["protocol_version"] = block.capture.protocolVersion;
    if (block.capture.protocolVersion == 2) {
        metadata["boot_counter"] = block.capture.bootCounter;
        metadata["counter_generation"] = block.capture.counterGeneration;
        JsonObject timing = metadata["timing"].to<JsonObject>();
        timing["source"] = "ntp";
        timing["sync_epoch_us"] = block.capture.syncEpochUs;
        timing["time_reference"] = "nominal_sample_clock";
        timing["uncertainty_us"] = nullptr;
    }
    metadata["device_id"] = deviceId;
    metadata["session_id"] = block.capture.sessionId;
    metadata["sequence"] = block.sequence;
    metadata["session_start_us"] = block.capture.sessionStartUs;
    metadata["first_frame"] = block.firstFrame;
    metadata["frame_count"] = block.frames;
    metadata["sample_rate"] = block.sampleRate;
    if (block.capture.signalProfile == greenmind::SignalProfile::Ad8232Raw400V1)
        metadata["signal_profile"] = greenmind::raw400Profile;
    metadata["channels"] = block.channels;
    metadata["sample_bits"] = block.sampleBits;
    JsonArray labels = metadata["channel_labels"].to<JsonArray>();
    for (uint8_t i = 0; i < block.channels; ++i) labels.add(String("CH") + String(i + 1));
    metadata["calibration_version"] = block.sampleBits == 16 ? "unsigned-mv-linear-int16-v1" : "raw-adc-counts-v1";
    metadata["firmware_version"] = greenmind::cloud::firmwareVersion;
    metadata["payload_sha256"] = digest;
    String header;
    serializeJson(metadata, header);
    if (header.length() > 2048) {
        Serial.println("direct_metadata_too_large retained=1");
        directStatus.record(false, -1, millis(), 0);
        return false;
    }
    transport.setCACert(certificate.c_str());
    transport.setHandshakeTimeout(10);
    if (!http.begin(transport, endpoint)) {
        directStatus.record(false, -1, millis(), 0);
        return false;
    }
    http.setReuse(true);
    http.setConnectTimeout(5000);
    http.setTimeout(10000);
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    http.addHeader("Authorization", String("Bearer ") + token);
    http.addHeader("Content-Type", "application/octet-stream");
    http.addHeader("X-GreenMind-Metadata", header);
    const bool reused = transport.connected();
    const uint32_t started = millis();
    int status = http.POST(const_cast<uint8_t*>(block.payload), block.byteCount());
    bool acknowledged = false;
    if ((status == 200 || status == 201) && http.getSize() >= 0 && http.getSize() <= 1024) {
        JsonDocument ack;
        if (!deserializeJson(ack, http.getString())) {
            acknowledged = greenmind::directAcknowledged(ack.as<JsonVariantConst>(),
                deviceId.c_str(), block.capture.sessionId, block.sequence, digest.c_str());
        }
    }
    directStatus.record(acknowledged, status, millis(), millis() - started);
    // Never print the URL, authorization header, response body or provisioning.
    Serial.printf("direct_upload status=%d ack=%d sequence=%llu elapsed_ms=%lu reused=%d heap=%u\n",
        status, acknowledged, block.sequence, millis() - started, reused, ESP.getFreeHeap());
    // Error bodies can be unread or malformed: discard that connection, while
    // retaining the same pending block for the existing idempotent retry path.
    if (!acknowledged) transport.stop();
    http.end();
    return acknowledged;
}

static bool sendGateway(const SampleBlock& block) {
    if (WiFi.status() != WL_CONNECTED || block.channels != 1 || block.sampleRate != 380 || block.sampleBits != 16) return false;
    JsonDocument body;
    body["mac_address"] = WiFi.macAddress();
    body["sample_rate"] = 380;
    // Match the accepted compact v3 Gateway contract, including stable identity.
    body["protocol_version"] = 3;
    body["boot_id"] = static_cast<uint32_t>(strtoul(sessionId, nullptr, 16));
    body["sequence"] = static_cast<uint32_t>(block.sequence);
    if (hasClock.load(std::memory_order_acquire))
        body["captured_at_epoch_ms"] = (sessionStartUs + (block.firstFrame + block.frames) * 1000000ULL / 380) / 1000;
    body["kind"] = "bio_signal";
    body["unit"] = "mV";
    body["value_scale_mv"] = 0.1;
    JsonArray samples = body["values_deci_mv"].to<JsonArray>();
    for (uint16_t i = 0; i < block.frames; ++i) samples.add(block.gatewayDeciMv[i]);
    String payload;
    serializeJson(body, payload);
    HTTPClient http;
    http.begin(gateway + "/api/v1/ingest");
    http.setConnectTimeout(3000);
    http.setTimeout(5000);
    http.addHeader("Content-Type", "application/json");
    const uint32_t started = millis();
    int status = http.POST(payload);
    bool acknowledged = false;
    if ((status == 200 || status == 201) && http.getSize() >= 0 && http.getSize() <= 1024) {
        JsonDocument ack;
        if (!deserializeJson(ack, http.getString())) {
            acknowledged = greenmind::gatewayAcknowledged(ack.as<JsonVariantConst>(),
                static_cast<uint32_t>(strtoul(sessionId, nullptr, 16)),
                static_cast<uint32_t>(block.sequence), block.frames);
        }
    }
    gatewayStatus.record(acknowledged, status, millis(), millis() - started);
    http.end();
    return acknowledged;
}

#include "DirectOta.h"

static bool setupAllowed() {
#ifdef GREENMIND_DIRECT_OTA
    return !otaBusy.load();
#else
    return true;
#endif
}

#include "DemandLiveNetwork.h"

static void directWorker(void*) {
    uint32_t backoff = 1000;
    for (;;) {
        // OTA may drain directPending, so it runs only outside retained retries.
        directOtaTick();
        if (!directQueue.peek(directPending)) {
            directLiveTick(false);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        while (!sendDirect(directPending)) {
            const uint32_t started = millis(), wait = backoff + esp_random() % 500;
            backoff = min(uint32_t(60000), backoff * 2);
            while (millis() - started < wait) {
                directLiveTick(true);
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }
        if (!directQueue.acknowledge(directPending.sequence)) {
            Serial.println("direct_spool_ack_invariant_failed");
            for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
        }
        backoff = 1000;
        directLiveTick(directQueue.used() != 0);
    }
}

static void gatewayWorker(void*) {
    uint32_t backoff = 1000;
    for (;;) {
        if (!gatewayQueue.pop(gatewayPending)) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }
        while (!sendGateway(gatewayPending)) {
            vTaskDelay(pdMS_TO_TICKS(backoff));
            backoff = min(uint32_t(60000), backoff * 2);
        }
        backoff = 1000;
    }
}

static bool startContinuousAdc() {
    adc_digi_init_config_t buffers{};
    buffers.max_store_buf_size = 16 * 1024;
    buffers.conv_num_each_intr = 64;
    buffers.adc1_chan_mask = 1U << ADC1_CHANNEL_3; // ESP32-S3 GPIO4
    if (adc_digi_initialize(&buffers) != ESP_OK) return false;
    adc_digi_pattern_config_t pattern{};
    pattern.atten = ADC_ATTEN_DB_11;
    pattern.channel = ADC1_CHANNEL_3;
    pattern.unit = 0; // DMA pattern units are zero-based (ADC1).
    pattern.bit_width = 12;
    adc_digi_configuration_t config{};
    config.pattern_num = 1;
    config.adc_pattern = &pattern;
    config.sample_freq_hz = greenmind::MainsAverage20ms::rawHz;
    config.conv_mode = ADC_CONV_SINGLE_UNIT_1;
    config.format = ADC_DIGI_OUTPUT_FORMAT_TYPE2;
    if (adc_digi_controller_configure(&config) == ESP_OK && adc_digi_start() == ESP_OK)
        return true;
    adc_digi_deinitialize();
    return false;
}

static void stopContinuousAdc() {
    adcHealthy.store(false);
    adc_digi_stop();
    adc_digi_deinitialize();
}

#include "Raw400Acquisition.h"

static void acquisition(void*) {
    if (mode == Mode::Direct) { acquireRaw400(); return; }
    uint64_t initialSyncEpoch = 0, initialSyncMonotonic = 0;
    if (mode == Mode::Direct || mode == Mode::Dual) {
        while (!syncSnapshot(initialSyncEpoch, initialSyncMonotonic))
            vTaskDelay(pdMS_TO_TICKS(50));
    }
    // DMA preserves real 500 us conversions across task yields and Wi-Fi work.
    // Timestamp the filtered output at its right edge: nominal delay 9.75 ms.
    constexpr uint64_t warmupUs = (greenmind::MainsAverage20ms::samples - 1) * 500ULL;
    uint64_t captureOrigin = 0;
    uint64_t origin = 0;
    uint64_t rawIndex = 0, frame = 0;
    greenmind::MainsAverage20ms average;
    alignas(4) uint8_t dma[256];
    bool running = false;
    for (;;) {
#ifdef GREENMIND_DIRECT_OTA
        if (otaPauseRequested.load()) {
            if (acquisitionBlock.frames) { submit(acquisitionBlock); acquisitionBlock.frames = 0; }
            if (running) { stopContinuousAdc(); running = false; }
            otaAcquisitionPaused.store(true);
            while (otaPauseRequested.load()) vTaskDelay(pdMS_TO_TICKS(10));
            otaAcquisitionPaused.store(false);
            // A fresh, full window follows the explicit OTA maintenance gap.
        }
#endif
        if (!running) {
            captureOrigin = esp_timer_get_time();
            if (!startContinuousAdc()) {
                adcFault.store(true);
                Serial.println("adc_start_failed acquisition_paused=1");
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            if (!origin) {
                origin = captureOrigin + warmupUs;
                liveCaptureOrigin(origin, 380, 2000);
                if (mode == Mode::Direct || mode == Mode::Dual) {
                    // Estimate from the SNTP-confirmed monotonic mapping, never
                    // from upload time or a later system-clock adjustment.
                    if (!greenmind::sampleClockStart(initialSyncEpoch, initialSyncMonotonic,
                                                     origin, sessionStartUs)) {
                        stopContinuousAdc(); origin = 0; adcFault.store(true);
                        Serial.println("direct_sample_clock_invalid acquisition_paused=1");
                        vTaskDelay(pdMS_TO_TICKS(1000));
                        continue;
                    }
                    captureMetadata.sessionStartUs = sessionStartUs;
                    captureMetadata.syncEpochUs = initialSyncEpoch;
                    hasClock.store(true, std::memory_order_release);
                }
            }
            running = true;
            adcHealthy.store(true); adcFault.store(false);
            average.reset(); rawIndex = 0;
        }
        if (mode == Mode::Gateway && !hasClock.load(std::memory_order_relaxed)) {
            uint64_t epoch, monotonic;
            if (syncSnapshot(epoch, monotonic)) {
                sessionStartUs = epoch + origin - monotonic;
                captureMetadata.sessionStartUs = sessionStartUs;
                captureMetadata.syncEpochUs = epoch;
                hasClock.store(true, std::memory_order_release);
            }
        }
        uint32_t bytes = 0;
        const esp_err_t result = adc_digi_read_bytes(dma, sizeof(dma), &bytes, 100);
        if (result != ESP_OK || bytes % sizeof(adc_digi_output_data_t)) {
            if (acquisitionBlock.frames) { submit(acquisitionBlock); acquisitionBlock.frames = 0; }
            stopContinuousAdc(); running = false;
            adcFault.store(true);
            adcRestarts.fetch_add(1);
            Serial.printf("adc_restart reason=%s real_gap=1\n", esp_err_to_name(result));
            // Never fill an overflow/timeout with repeated readings. The next
            // full real window advances the existing frame clock across the gap.
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        for (uint32_t offset = 0; offset < bytes; offset += sizeof(adc_digi_output_data_t)) {
            const auto* sample = reinterpret_cast<const adc_digi_output_data_t*>(dma + offset);
            if (sample->type2.channel != ADC1_CHANNEL_3 || sample->type2.unit != 0) continue;
            const uint16_t raw = sample->type2.data;
            const uint64_t sampledAt = captureOrigin + rawIndex++ * 500ULL;
            liveRawTap(raw, sampledAt);
            rawConversions.fetch_add(1);
            if (raw == 0 || raw == 4095) rawClipped.fetch_add(1);
            if (!average.add(raw) || sampledAt < origin + frame * 1000000ULL / 380) continue;
            const uint64_t due = (sampledAt - origin) * 380 / 1000000ULL;
            if (due > frame) {
                if (acquisitionBlock.frames) { submit(acquisitionBlock); acquisitionBlock.frames = 0; }
                timingDropped.fetch_add(static_cast<uint32_t>(due - frame)); frame = due;
            }
        const uint16_t deciMv = static_cast<uint16_t>(std::lround(average.mean() * (33000.0 / 4095.0)));
        liveFrameTap(frame, deciMv * 0.1);
        SignalDisplayStats::record(deciMv);
        filteredFrames.fetch_add(1);
        if (!acquisitionBlock.frames) acquisitionBlock.firstFrame = frame;
        const uint16_t index = acquisitionBlock.frames++;
        acquisitionBlock.gatewayDeciMv[index] = deciMv;
        const int16_t pcm = greenmind::legacyPcm(deciMv * 0.1);
        acquisitionBlock.payload[index * 2] = static_cast<uint8_t>(pcm);
        acquisitionBlock.payload[index * 2 + 1] = static_cast<uint8_t>(uint16_t(pcm) >> 8);
        ++frame;
        if (acquisitionBlock.frames == 380) { submit(acquisitionBlock); acquisitionBlock.frames = 0; }
        }
        // The hardware continues acquisition during this scheduler yield.
        vTaskDelay(1);
    }
}

static void provision(const String& line) {
    JsonDocument doc;
    if (deserializeJson(doc, line)) { Serial.println("provision_invalid_json"); return; }
    Mode requested = greenmind::parseMode(doc["transport"] | "GATEWAY");
    String url = doc["endpoint"] | "";
    String ca = doc["ca"] | "";
    String gw = doc["gateway"] | "";
    if (requested == Mode::Invalid || String(doc["ssid"] | "").isEmpty() ||
        ((requested == Mode::Direct || requested == Mode::Dual) &&
         (!greenmind::cloud::acceptsEndpoint(url.c_str()) ||
          ca.indexOf("BEGIN CERTIFICATE") < 0 || String(doc["token"] | "").length() != 80 ||
          String(doc["device_id"] | "").length() != 36)) ||
        ((requested == Mode::Gateway || requested == Mode::Dual) && !gw.startsWith("http://"))) {
        Serial.println("provision_invalid_configuration"); return;
    }
    preferences.begin(greenmind::cloud::preferencesNamespace, false);
    for (const char* key : {"transport", "ssid", "password", "endpoint", "token", "device_id", "ca", "gateway"})
        preferences.putString(key, doc[key] | "");
    preferences.remove("pair_code");
    preferences.end();
    Serial.println("provision_saved_restarting");
    delay(100);
    ESP.restart();
}

static bool prepareDirectSpool() {
    constexpr size_t reserve = 1024 * 1024;
    const size_t available = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    void* memory = nullptr;
    uint32_t bytes = 4 * 1024 * 1024;
    if (available > reserve) {
        while (bytes > largest || bytes > available - reserve) bytes /= 2;
        if (bytes >= 128 * 1024)
            memory = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    directPsram = memory != nullptr;
    if (!memory) {
        // Explicitly degraded compatibility for non-PSRAM boards. No 120s claim.
        bytes = 32768;
        if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >= bytes + 100 * 1024)
            memory = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!memory || !directQueue.begin(memory, bytes)) {
        if (memory) heap_caps_free(memory);
        return false;
    }
    Serial.printf("direct_spool kind=%s bytes=%u nominal_capacity_s=%u volatile=1\n",
        directPsram ? "PSRAM" : "RAM_LIMITED", bytes, directQueue.capacitySeconds(mode == Mode::Direct ? 400 : 380));
    return true;
}

void setup() {
    Serial.begin(115200);
    Serial.printf("firmware=%s\n", greenmind::cloud::firmwareVersion);
#ifdef GREENMIND_DIRECT_ADC_SELFTEST
#ifdef GREENMIND_DIRECT_RAW400_SELFTEST
    mode = Mode::Direct; // Separate offline raw acquisition probe; no identity/network writes.
#else
    mode = Mode::Gateway; // Existing filtered probe remains available.
#endif
    StatusDisplay::init();
    xTaskCreatePinnedToCore(acquisition, "adc-selftest", 4096, nullptr, 2, nullptr, 1);
    return;
#endif
#ifdef GREENMIND_DIRECT_OTA
    otaBootSetup();
    xTaskCreatePinnedToCore(otaBootGuard, "ota-boot-guard", 4096, nullptr, 1, nullptr, 0);
#endif
    StatusDisplay::init();
    makeSessionId();
    memcpy(captureMetadata.sessionId, sessionId, sizeof(sessionId));
    captureMetadata.protocolVersion = GREENMIND_DIRECT_PROTOCOL_VERSION;
    preferences.begin(greenmind::cloud::preferencesNamespace, true);
    mode = greenmind::parseMode(preferences.getString("transport", "GATEWAY").c_str());
    ssid = preferences.getString("ssid");
    password = preferences.getString("password");
    endpoint = preferences.getString("endpoint");
    token = preferences.getString("token");
    deviceId = preferences.getString("device_id");
    deviceId.toLowerCase();
    certificate = preferences.getString("ca");
    gateway = preferences.getString("gateway");
    pairingCode = preferences.getString("pair_code");
    preferences.end();
    pinMode(0, INPUT_PULLUP);
    const bool directValid = token.length() == 80 && deviceId.length() == 36 &&
        greenmind::cloud::acceptsEndpoint(endpoint.c_str()) && certificate.indexOf("BEGIN CERTIFICATE") >= 0;
    identityConfigured = pairingCode.isEmpty() &&
        ((mode == Mode::Direct && directValid) || (mode == Mode::Gateway && gateway.startsWith("http://")) ||
         (mode == Mode::Dual && directValid && gateway.startsWith("http://")));
    configured = !ssid.isEmpty() && identityConfigured;
    WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t) {
        if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) wifiGotIpGeneration.fetch_add(1);
    });
    WiFi.persistent(false); // Try new WLAN credentials in RAM before saving them.
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    if (!ssid.isEmpty()) WiFi.begin(ssid.c_str(), password.c_str());
    sntp_set_time_sync_notification_cb(onTimeSync);
    configTime(0, 0, "pool.ntp.org", "time.cloudflare.com");
    if (!configured) { manageSetupPortal(); return; }
    if ((mode == Mode::Direct || mode == Mode::Dual) && !allocateBootIdentity()) {
        Serial.println("direct_boot_identity_failed acquisition_paused=1");
        StatusDisplay::show("SPEICHERFEHLER", "Keine Aufnahme", "Bootzaehler fehlt",
            "USB-Diagnose pruefen", "Neustart erforderlich");
        configured = false;
        return;
    }
    if ((mode == Mode::Direct || mode == Mode::Dual) && !prepareDirectSpool()) {
        Serial.println("direct_spool_allocation_failed");
        StatusDisplay::show("PUFFERFEHLER", "Keine Aufnahme", "Speicher fehlt",
            "Konfiguration pruefen", "Neustart erforderlich");
        configured = false;
        return;
    }
    StatusDisplay::show("START", "WLAN verbinden...", greenmind::cloud::host, "Warte auf Verbindung", "");
    if (mode == Mode::Direct || mode == Mode::Dual)
        xTaskCreatePinnedToCore(directWorker, "direct-upload", 12288, nullptr, 1, nullptr, 0);
    if (mode == Mode::Gateway || mode == Mode::Dual)
        xTaskCreatePinnedToCore(gatewayWorker, "gateway-upload", 12288, nullptr, 1, nullptr, 0);
    xTaskCreatePinnedToCore(acquisition, "adc-acquisition", 4096, nullptr, 2, nullptr, 1);
}

static void updateStatusDisplay() {
    static uint32_t lastDraw = 0;
    if (millis() - lastDraw < 500) return;
    lastDraw = millis();
    if (portalActive) {
        StatusDisplay::show("SETUP", portalName, "192.168.4.1", portalScreenMessage,
            pairPending ? "Bitte warten..." : "Mit Handy verbinden");
    } else if (configured) {
#ifdef GREENMIND_DIRECT_OTA
        if (otaBusy.load()) {
            StatusDisplay::show("FIRMWAREUPDATE", "Aufnahme pausiert", "Puffer wird bestaetigt",
                "Firmware wird geprueft", "Danach Neustart");
            return;
        }
#endif
        if (adcFault.load()) {
            StatusDisplay::show("ADC PAUSIERT", "Keine neuen Messwerte", "USB-Diagnose pruefen", "WLAN bleibt aktiv", "Keine Ersatzwerte");
            return;
        }
        const bool wifiOk = WiFi.status() == WL_CONNECTED;
        // In DUAL mode show both independent paths, six seconds per page.
        const bool cloudPage = mode == Mode::Direct ||
            (mode == Mode::Dual && (millis() / 6000) % 2 == 0);
        const auto stats = (cloudPage ? directStatus : gatewayStatus).snapshot();
        const uint32_t lost = (cloudPage ? directDropped.load() : gatewayDropped.load()) + timingDropped.load();
        const uint32_t now = millis();
        const auto voltage = SignalDisplayStats::read();
        static String sensorId;
        if (sensorId.isEmpty()) {
            sensorId = WiFi.macAddress(); sensorId.replace(":", "");
            sensorId = sensorId.substring(sensorId.length() - 4); sensorId.toLowerCase();
        }
        StatusDisplay::showTelemetry(cloudPage, mode == Mode::Dual, wifiOk,
            !cloudPage || hasClock.load(), wifiOk ? WiFi.RSSI() : 0, stats, lost, now,
            voltage.meanDeciMv, voltage.minDeciMv, voltage.maxDeciMv,
            voltage.fresh(now), sensorId,
            directQueue.used(), directQueue.capacity(), directPsram);
    }
}

static void printAdcStats() {
    if (mode == Mode::Direct) { printRaw400Stats(); return; }
    const auto voltage = SignalDisplayStats::read();
    Serial.printf("adc_filter=avg20ms raw_hz=2000 output_hz=380 raw_samples=%u raw_clipped=%u output_frames=%u restarts=%u healthy=%d mean_decimv=%u min_decimv=%u max_decimv=%u\n",
        rawConversions.exchange(0), rawClipped.exchange(0), filteredFrames.exchange(0),
        adcRestarts.load(), adcHealthy.load(), voltage.meanDeciMv, voltage.minDeciMv, voltage.maxDeciMv);
}

void loop() {
#ifdef GREENMIND_DIRECT_ADC_SELFTEST
    static uint32_t lastTest = 0;
    if (millis() - lastTest >= 10000) { lastTest = millis(); printAdcStats(); }
    delay(10);
    return;
#endif
    manageSetupPortal();
    handlePortal();
    updateStatusDisplay();
    // Hold BOOT for five seconds to change WLAN via the local portal.
    static uint32_t heldSince = 0;
    static bool reopenPortal = false;
    if (digitalRead(0) == LOW) {
        if (!heldSince) heldSince = millis();
        if (millis() - heldSince > 5000 && !portalActive) reopenPortal = true;
    } else {
        heldSince = 0;
        if (reopenPortal) {
            reopenPortal = false;
            if (setupAllowed()) {
                setupWindow.requestManual(millis());
                startPortal();
            }
        }
    }
    while (Serial.available()) {
        char value = static_cast<char>(Serial.read());
        if (value == '\n') { provision(provisionLine); provisionLine = ""; }
        else if (provisionLine.length() < 8192) provisionLine += value;
        else { provisionLine = ""; Serial.println("provision_line_limit"); }
    }
    static uint32_t lastStatus = 0;
    if (millis() - lastStatus > 10000) {
        lastStatus = millis();
        Serial.printf("sensor_alive uptime_ms=%lu display=%s hotspot=%d clients=%u wifi=%d\n",
            millis(), StatusDisplay::ready() ? "ready" : "unavailable", portalActive,
            portalActive ? WiFi.softAPgetStationNum() : 0, WiFi.status());
        printAdcStats();
        Serial.printf("direct_live active=%d packets=%u preview_dropped=%u http_errors=%u max_http_ms=%u\n",
            greenmind::liveDemand(millis(), liveUntil.load()), livePackets.exchange(0),
            liveDropped.exchange(0), liveHttpErrors.exchange(0), liveHttpMaximumMs.exchange(0));
        Serial.printf("direct_dropped=%u gateway_dropped=%u timing_dropped=%u\n",
            directDropped.load(), gatewayDropped.load(), timingDropped.load());
        Serial.printf("direct_spool_used=%u capacity=%u nominal_capacity_s=%u psram=%d\n",
            directQueue.used(), directQueue.capacity(), directQueue.capacitySeconds(mode == Mode::Direct ? 400 : 380), directPsram);
        const auto status = directStatus.snapshot();
        Serial.printf("display_upload acks=%u interval_ms=%u age_ms=%u errors=%u http=%d\n",
            status.acknowledgements, status.intervalMs,
            status.acknowledgements ? millis() - status.lastAckMs : 0,
            status.failures, status.result);
    }
    delay(10);
}
