/**
 * Explicit Direct/DUAL firmware for new/provisioned Biolingo ESP32-S3 devices.
 * OTA is explicit on the N16R8 target; USB bootstrap retains pairing.
 * ADC input: GPIO4 at hardware-clocked 2 kHz; 20 ms moving mean, 380 Hz PCM16.
 * ADS131M04 drivers can submit interleaved SampleBlock PCM24 independently.
 */
#include <Arduino.h>
#include <driver/adc.h>
#include "../../../transport_validation/MainsAverage20ms.h"
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <mbedtls/sha256.h>
#include <sys/time.h>

#include "../../../direct_transport/DirectCore.h"
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
static std::atomic<bool> hasClock{false};
static uint64_t sequence = 0;
static greenmind::PcmSpool directQueue;
static bool directPsram = false;
static greenmind::SpscQueue<SampleBlock, 4> gatewayQueue;
static SampleBlock acquisitionBlock, directPending, gatewayPending;
static std::atomic<uint32_t> directDropped{0}, gatewayDropped{0}, timingDropped{0};
static bool configured = false;
static greenmind::UploadStatus directStatus, gatewayStatus;
static String provisionLine;
// Display-only snapshot of the same mains-averaged sample sent to the cloud.
static std::atomic<uint32_t> rawConversions{0}, rawClipped{0}, adcRestarts{0}, filteredFrames{0};
static std::atomic<bool> adcHealthy{false}, adcFault{false};
#include "../../../transport_validation/SignalDisplayStats.h"

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
    static WiFiClientSecure transport;
    static HTTPClient http;
    if (WiFi.status() != WL_CONNECTED || !hasClock.load(std::memory_order_acquire)) return false;
    const String digest = sha256(block.payload, block.byteCount());
    JsonDocument metadata;
    metadata["protocol_version"] = 1;
    metadata["device_id"] = deviceId;
    metadata["session_id"] = sessionId;
    metadata["sequence"] = block.sequence;
    metadata["session_start_us"] = sessionStartUs;
    metadata["first_frame"] = block.firstFrame;
    metadata["frame_count"] = block.frames;
    metadata["sample_rate"] = block.sampleRate;
    metadata["channels"] = block.channels;
    metadata["sample_bits"] = block.sampleBits;
    JsonArray labels = metadata["channel_labels"].to<JsonArray>();
    for (uint8_t i = 0; i < block.channels; ++i) labels.add(String("CH") + String(i + 1));
    metadata["calibration_version"] = block.sampleBits == 16 ? "unsigned-mv-linear-int16-v1" : "raw-adc-counts-v1";
    metadata["firmware_version"] = greenmind::cloud::firmwareVersion;
    metadata["payload_sha256"] = digest;
    String header;
    serializeJson(metadata, header);
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
                deviceId.c_str(), sessionId, block.sequence, digest.c_str());
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

static void directWorker(void*) {
    uint32_t backoff = 1000;
    for (;;) {
        directOtaTick();
        if (!directQueue.peek(directPending)) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }
        while (!sendDirect(directPending)) {
            vTaskDelay(pdMS_TO_TICKS(backoff + esp_random() % 500));
            backoff = min(uint32_t(60000), backoff * 2);
        }
        if (!directQueue.acknowledge(directPending.sequence)) {
            // An internal ownership failure must never silently discard data.
            Serial.println("direct_spool_ack_invariant_failed");
            for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
        }
        backoff = 1000;
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

static void acquisition(void*) {
    if (mode == Mode::Direct) {
        while (WiFi.status() != WL_CONNECTED || time(nullptr) < 1700000000)
            vTaskDelay(pdMS_TO_TICKS(50));
    }
    // DMA preserves real 500 us conversions across task yields and Wi-Fi work.
    // Timestamp the filtered output at its right edge: nominal delay 9.75 ms.
    constexpr uint64_t warmupUs = (greenmind::MainsAverage20ms::samples - 1) * 500ULL;
    uint64_t captureOrigin = esp_timer_get_time();
    const uint64_t origin = captureOrigin + warmupUs;
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
            running = true;
            adcHealthy.store(true); adcFault.store(false);
            average.reset(); rawIndex = 0;
        }
        if (!hasClock.load(std::memory_order_relaxed)) {
            timeval wall; gettimeofday(&wall, nullptr);
            if (wall.tv_sec > 1700000000) {
                sessionStartUs = uint64_t(wall.tv_sec) * 1000000 + wall.tv_usec - (esp_timer_get_time() - origin);
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
            rawConversions.fetch_add(1);
            if (raw == 0 || raw == 4095) rawClipped.fetch_add(1);
            if (!average.add(raw) || sampledAt < origin + frame * 1000000ULL / 380) continue;
            const uint64_t due = (sampledAt - origin) * 380 / 1000000ULL;
            if (due > frame) {
                if (acquisitionBlock.frames) { submit(acquisitionBlock); acquisitionBlock.frames = 0; }
                timingDropped.fetch_add(static_cast<uint32_t>(due - frame)); frame = due;
            }
        const uint16_t deciMv = static_cast<uint16_t>(std::lround(average.mean() * (33000.0 / 4095.0)));
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
        directPsram ? "PSRAM" : "RAM_LIMITED", bytes, directQueue.capacitySeconds380());
    return true;
}

void setup() {
    Serial.begin(115200);
    Serial.printf("firmware=%s\n", greenmind::cloud::firmwareVersion);
#ifdef GREENMIND_DIRECT_ADC_SELFTEST
    mode = Mode::Gateway; // The production DIRECT network/time wait is untouched.
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
    configured = ssid.length() && pairingCode.isEmpty() &&
        ((mode == Mode::Direct && directValid) || (mode == Mode::Gateway && gateway.startsWith("http://")) ||
         (mode == Mode::Dual && directValid && gateway.startsWith("http://")));
    if (!configured) { startPortal(); return; }
    if ((mode == Mode::Direct || mode == Mode::Dual) && !prepareDirectSpool()) {
        Serial.println("direct_spool_allocation_failed");
        StatusDisplay::show("PUFFERFEHLER", "Keine Aufnahme", "Speicher fehlt",
            "Konfiguration pruefen", "Neustart erforderlich");
        configured = false;
        return;
    }
    StatusDisplay::show("START", "WLAN verbinden...", greenmind::cloud::host, "Warte auf Verbindung", "");
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(ssid.c_str(), password.c_str());
    configTime(0, 0, "pool.ntp.org", "time.cloudflare.com");
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
            preferences.begin(greenmind::cloud::preferencesNamespace, false);
            preferences.putString("ssid", "");
            preferences.end();
            ESP.restart();
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
        Serial.printf("direct_dropped=%u gateway_dropped=%u timing_dropped=%u\n",
            directDropped.load(), gatewayDropped.load(), timingDropped.load());
        Serial.printf("direct_spool_used=%u capacity=%u nominal_capacity_s=%u psram=%d\n",
            directQueue.used(), directQueue.capacity(), directQueue.capacitySeconds380(), directPsram);
        const auto status = directStatus.snapshot();
        Serial.printf("display_upload acks=%u interval_ms=%u age_ms=%u errors=%u http=%d\n",
            status.acknowledgements, status.intervalMs,
            status.acknowledgements ? millis() - status.lastAckMs : 0,
            status.failures, status.result);
    }
    delay(10);
}
