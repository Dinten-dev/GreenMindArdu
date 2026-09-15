/**
 * Explicit test firmware for new/provisioned Biolingo ESP32-S3 devices.
 * Existing production firmware is untouched. No OTA or automatic migration.
 * ADC input: Biolingo v22 AD8232 on GPIO4; 380 Hz, mono PCM16 comparison.
 * ADS131M04 drivers can submit interleaved SampleBlock PCM24 independently.
 */
#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_timer.h>
#include <mbedtls/sha256.h>
#include <sys/time.h>

#include "../../../direct_transport/DirectCore.h"
#include "StatusDisplay.h"

using greenmind::Mode;
using greenmind::SampleBlock;
static Preferences preferences;
static Mode mode = Mode::Gateway;
static String ssid, password, endpoint, token, deviceId, certificate, gateway, pairingCode;
static char sessionId[37];
static uint64_t sessionStartUs = 0;
static std::atomic<bool> hasClock{false};
static uint64_t sequence = 0;
static greenmind::SpscQueue<SampleBlock, 8> directQueue;
static greenmind::SpscQueue<SampleBlock, 4> gatewayQueue;
static SampleBlock acquisitionBlock, directPending, gatewayPending;
static std::atomic<uint32_t> directDropped{0}, gatewayDropped{0}, timingDropped{0};
static bool configured = false;
static std::atomic<uint32_t> lastDirectAckMs{0};
static std::atomic<bool> directAcknowledged{false};
static String provisionLine;

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
    metadata["firmware_version"] = "direct-hotspot-v2.4";
    metadata["payload_sha256"] = digest;
    String header;
    serializeJson(metadata, header);
    transport.setCACert(certificate.c_str());
    transport.setHandshakeTimeout(10);
    if (!http.begin(transport, endpoint)) return false;
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
            const String state = ack["status"].as<String>();
            acknowledged = (state == "persisted" || state == "duplicate") &&
                ack["device_id"].as<String>() == deviceId &&
                ack["session_id"].as<String>() == sessionId &&
                ack["sequence"].as<uint64_t>() == block.sequence &&
                ack["payload_sha256"].as<String>() == digest;
        }
    }
    if (acknowledged) {
        lastDirectAckMs.store(millis(), std::memory_order_relaxed);
        directAcknowledged.store(true, std::memory_order_release);
    }
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
    int status = http.POST(payload);
    bool acknowledged = false;
    if ((status == 200 || status == 201) && http.getSize() >= 0 && http.getSize() <= 1024) {
        JsonDocument ack;
        if (!deserializeJson(ack, http.getString())) {
            acknowledged = ack["sequence"].as<uint32_t>() == static_cast<uint32_t>(block.sequence) &&
                ack["samples_archived"].as<uint32_t>() == block.frames &&
                (ack["status"] == "queued" || ack["status"] == "duplicate");
        }
    }
    http.end();
    return acknowledged;
}

static void directWorker(void*) {
    uint32_t backoff = 1000;
    for (;;) {
        if (!directQueue.pop(directPending)) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }
        while (!sendDirect(directPending)) {
            vTaskDelay(pdMS_TO_TICKS(backoff + esp_random() % 500));
            backoff = min(uint32_t(60000), backoff * 2);
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

static void acquisition(void*) {
    // A Direct-only session starts once its timestamps and network are ready.
    // Boot-time Wi-Fi/NTP waits must not fill the finite measurement queue.
    // Gateway and DUAL retain their independent, immediate acquisition path.
    if (mode == Mode::Direct) {
        while (WiFi.status() != WL_CONNECTED || time(nullptr) < 1700000000)
            vTaskDelay(pdMS_TO_TICKS(50));
    }
    uint64_t origin = esp_timer_get_time();
    uint64_t frame = 0;
    analogReadResolution(12);
    analogSetPinAttenuation(4, ADC_11db);
    for (;;) {
        const uint64_t now = esp_timer_get_time();
        if (!hasClock.load(std::memory_order_relaxed)) {
            // Anchor this session once UTC becomes available. Gateway can run
            // without UTC; Direct retains its pending RAM blocks until then.
            timeval wall;
            gettimeofday(&wall, nullptr);
            if (wall.tv_sec > 1700000000) {
                sessionStartUs = uint64_t(wall.tv_sec) * 1000000 + wall.tv_usec - (esp_timer_get_time() - origin);
                hasClock.store(true, std::memory_order_release);
            }
        }
        const uint64_t target = origin + frame * 1000000ULL / 380;
        if (now < target) { delayMicroseconds(static_cast<uint32_t>(target - now)); continue; }
        if (now > target + 1000000ULL / 380) {
            if (acquisitionBlock.frames) { submit(acquisitionBlock); acquisitionBlock.frames = 0; }
            const uint64_t missed = (now - target) * 380 / 1000000;
            frame += missed;
            timingDropped.fetch_add(static_cast<uint32_t>(missed));
        }
        const uint16_t deciMv = static_cast<uint16_t>(std::lround(analogRead(4) * (33000.0 / 4095.0)));
        if (!acquisitionBlock.frames) acquisitionBlock.firstFrame = frame;
        const uint16_t index = acquisitionBlock.frames++;
        acquisitionBlock.gatewayDeciMv[index] = deciMv;
        const int16_t pcm = greenmind::legacyPcm(deciMv * 0.1);
        acquisitionBlock.payload[index * 2] = static_cast<uint8_t>(pcm);
        acquisitionBlock.payload[index * 2 + 1] = static_cast<uint8_t>(uint16_t(pcm) >> 8);
        ++frame;
        if (acquisitionBlock.frames == 380) { submit(acquisitionBlock); acquisitionBlock.frames = 0; }
        // Yield without network involvement; missed deadlines are explicit above.
        if (frame % 38 == 0) vTaskDelay(1);
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
         (!url.startsWith("https://") || !url.endsWith("/api/v1/direct-ingest/chunks") ||
          ca.indexOf("BEGIN CERTIFICATE") < 0 || String(doc["token"] | "").length() != 80 ||
          String(doc["device_id"] | "").length() != 36)) ||
        ((requested == Mode::Gateway || requested == Mode::Dual) && !gw.startsWith("http://"))) {
        Serial.println("provision_invalid_configuration"); return;
    }
    preferences.begin("gmdirect", false);
    for (const char* key : {"transport", "ssid", "password", "endpoint", "token", "device_id", "ca", "gateway"})
        preferences.putString(key, doc[key] | "");
    preferences.remove("pair_code");
    preferences.end();
    Serial.println("provision_saved_restarting");
    delay(100);
    ESP.restart();
}

void setup() {
    Serial.begin(115200);
    Serial.println("firmware=direct-hotspot-v2.4");
    StatusDisplay::init();
    makeSessionId();
    preferences.begin("gmdirect", true);
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
        endpoint.startsWith("https://") && certificate.indexOf("BEGIN CERTIFICATE") >= 0;
    configured = ssid.length() && pairingCode.isEmpty() &&
        ((mode == Mode::Direct && directValid) || (mode == Mode::Gateway && gateway.startsWith("http://")) ||
         (mode == Mode::Dual && directValid && gateway.startsWith("http://")));
    if (!configured) { startPortal(); return; }
    StatusDisplay::show("START", "WLAN verbinden...", "test.green-mind.ch", "Warte auf Verbindung", "");
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
    if (millis() - lastDraw < 1000) return;
    lastDraw = millis();
    if (portalActive) {
        StatusDisplay::show("SETUP", portalName, "192.168.4.1", portalScreenMessage,
            pairPending ? "Bitte warten..." : "Mit Handy verbinden");
    } else if (configured) {
        const bool wifiOk = WiFi.status() == WL_CONNECTED;
        const bool recentAck = directAcknowledged.load(std::memory_order_acquire) &&
            millis() - lastDirectAckMs.load(std::memory_order_relaxed) < 30000;
        const char* state = !wifiOk ? "WLAN: verbinden..." : !hasClock.load() ? "Uhrzeit wird gesetzt" :
            mode == Mode::Gateway ? "Gateway-Modus" : recentAck ? "Cloud: Daten OK" : "Cloud: warte auf ACK";
        StatusDisplay::show("SENSOR", state, "test.green-mind.ch",
            "Verlust: " + String(directDropped.load() + timingDropped.load()), "BOOT 5s: WLAN-Setup");
    }
}

void loop() {
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
            preferences.begin("gmdirect", false);
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
        Serial.printf("direct_dropped=%u gateway_dropped=%u timing_dropped=%u\n",
            directDropped.load(), gatewayDropped.load(), timingDropped.load());
    }
    delay(10);
}
