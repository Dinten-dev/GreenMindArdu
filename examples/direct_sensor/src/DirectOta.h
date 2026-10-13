#pragma once

// Included after the Direct uploader: one consumer owns uploads and OTA.
#ifdef GREENMIND_DIRECT_OTA
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp32s3/spiram.h>
#include "../../../direct_transport/DirectOtaContract.h"

static std::atomic<bool> otaPauseRequested{false}, otaAcquisitionPaused{false};
static std::atomic<bool> otaBootHealthy{false};
static std::atomic<bool> otaBusy{false};
static String otaPendingCommand, otaPendingVersion;
static bool otaTrialBoot = false;
// Arduino's default confirms a trial before setup; postpone real acceptance.
extern "C" bool verifyRollbackLater() { return true; }

static void otaBootSetup() {
    const auto* running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    otaTrialBoot = esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY;
    Preferences prefs;
    prefs.begin("gmdirectota", true);
    otaPendingCommand = prefs.getString("pending_id");
    otaPendingVersion = prefs.getString("pending_ver");
    prefs.end();
    Serial.printf("direct_ota_boot slot=%s offset=0x%lx trial=%d version=%s\n",
        running->label, running->address, otaTrialBoot, greenmind::cloud::firmwareVersion);
    Serial.printf("direct_ota_hardware psram_physical_bytes=%u psram_heap_bytes=%u flash_bytes=%u\n",
        unsigned(esp_spiram_get_size()), ESP.getPsramSize(), ESP.getFlashChipSize());
}

static void otaBootGuard(void*) {
    if (otaTrialBoot) {
        const uint32_t started = millis();
        // Fault-injection build exists only for the local hardware rollback test.
#ifdef GREENMIND_OTA_TEST_REJECT_BOOT
        constexpr uint32_t deadline = 15000;
#else
        constexpr uint32_t deadline = 180000;
#endif
        while (!otaBootHealthy.load() && millis() - started < deadline) vTaskDelay(pdMS_TO_TICKS(100));
        if (!otaBootHealthy.load()) {
            Serial.println("direct_ota_boot_failed rollback_requested volatile_buffer_may_be_lost=1");
            esp_ota_mark_app_invalid_rollback_and_reboot();
        }
    }
    vTaskDelete(nullptr);
}

static bool otaHttpBegin(HTTPClient& http, WiFiClientSecure& tls, const String& path) {
    if (WiFi.status() != WL_CONNECTED || !hasClock.load()) return false;
    tls.setCACert(certificate.c_str());
    tls.setHandshakeTimeout(5);
    if (!http.begin(tls, String(greenmind::cloud::base) + "/ota" + path)) return false;
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    http.setConnectTimeout(5000);
    http.setTimeout(5000);
    http.setReuse(false);
    http.addHeader("Authorization", String("Bearer ") + token);
    return true;
}

static bool otaReport(const String& command, const char* status, const char* error = "") {
    HTTPClient http;
    WiFiClientSecure tls;
    if (!otaHttpBegin(http, tls, "/report")) return false;
    http.addHeader("Content-Type", "application/json");
    JsonDocument doc;
    doc["device_id"] = deviceId;
    doc["command_id"] = command;
    doc["profile"] = greenmind::directOtaProfile;
    doc["firmware_version"] = greenmind::cloud::firmwareVersion;
    doc["status"] = status;
    doc["error"] = error;
    doc["trial_boot"] = otaTrialBoot;
    doc["slot"] = esp_ota_get_running_partition()->label;
    doc["spool_bytes"] = directQueue.used();
    doc["acknowledgements"] = directStatus.snapshot().acknowledgements;
    String payload; serializeJson(doc, payload);
    const int result = http.POST(payload);
    http.end();
    Serial.printf("direct_ota_report command=%s status=%s http=%d\n", command.c_str(), status, result);
    return result == 200;
}

static bool otaDownload(const String& command, const String& digest, uint32_t bytes, const char*& failure) {
    const auto* partition = esp_ota_get_next_update_partition(nullptr);
    if (!partition || bytes > partition->size) { failure = "partition"; return false; }
    HTTPClient http;
    WiFiClientSecure tls;
    if (!otaHttpBegin(http, tls, String("/download/") + command)) { failure = "connect"; return false; }
    http.useHTTP10(true);
    if (http.GET() != 200 || http.getSize() != static_cast<int>(bytes)) {
        failure = "length_or_http"; http.end(); return false;
    }
    uint8_t block[2048];
    auto* stream = http.getStreamPtr();
    size_t total = 0;
    uint32_t lastProgress = millis(), started = millis();
    esp_ota_handle_t handle = 0;
    mbedtls_sha256_context context;
    mbedtls_sha256_init(&context);
    bool ok = mbedtls_sha256_starts_ret(&context, 0) == 0;
    while (ok && total < bytes) {
        const int available = stream->available();
        if (available <= 0) {
            if (!stream->connected() || millis() - lastProgress > 10000 || millis() - started > 90000) {
                failure = "truncated_or_timeout"; ok = false; break;
            }
            vTaskDelay(pdMS_TO_TICKS(1)); continue;
        }
        const size_t count = !total ? 24 : min(size_t(available), min(sizeof(block), size_t(bytes - total)));
        const size_t read = stream->readBytes(block, count);
        if (!read) continue;
        if (!total) {
            if (!greenmind::otaImageHeader(block, read)) { failure = "wrong_chip"; ok = false; break; }
            if (esp_ota_begin(partition, bytes, &handle) != ESP_OK) { failure = "flash_begin"; ok = false; break; }
        }
        if (esp_ota_write(handle, block, read) != ESP_OK ||
            mbedtls_sha256_update_ret(&context, block, read) != 0) {
            failure = "flash_write"; ok = false; break;
        }
        total += read; lastProgress = millis();
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    uint8_t hash[32];
    char hex[65] = {};
    const int excess = stream->available();
    if (ok && total == bytes && excess <= 0 && mbedtls_sha256_finish_ret(&context, hash) == 0) {
        for (size_t i = 0; i < 32; ++i) snprintf(hex + 2 * i, 3, "%02x", hash[i]);
        ok = digest == hex;
        if (!ok) failure = "sha256";
    } else {
        ok = false; if (!failure) failure = "stream";
        Serial.printf("direct_ota_stream_failed bytes=%u expected=%u excess=%d\n", unsigned(total), bytes, excess);
    }
    mbedtls_sha256_free(&context);
    http.end();
    if (!ok) { if (handle) esp_ota_abort(handle); return false; }
    // esp_ota_end validates the complete ESP image without selecting its boot slot.
    if (esp_ota_end(handle) != ESP_OK) { failure = "invalid_image"; return false; }
    // Independently read all flashed bytes before allowing a boot switch.
    mbedtls_sha256_init(&context);
    ok = mbedtls_sha256_starts_ret(&context, 0) == 0;
    for (size_t offset = 0; ok && offset < bytes; offset += sizeof(block)) {
        const size_t count = min(sizeof(block), size_t(bytes - offset));
        ok = esp_partition_read(partition, offset, block, count) == ESP_OK &&
            mbedtls_sha256_update_ret(&context, block, count) == 0;
    }
    ok = ok && mbedtls_sha256_finish_ret(&context, hash) == 0;
    mbedtls_sha256_free(&context);
    if (ok) {
        for (size_t i = 0; i < 32; ++i) snprintf(hex + 2 * i, 3, "%02x", hash[i]);
        ok = digest == hex;
    }
    if (!ok) { failure = "flash_readback_sha256"; return false; }
    Serial.printf("direct_ota_verified command=%s bytes=%u sha256=%s slot=%s readback=1\n",
        command.c_str(), bytes, hex, partition->label);
    return true;
}

static void directOtaTick() {
    // Arduino reports usable heap, smaller than the physical 8-MiB chip.
    if (mode != Mode::Direct || !directPsram || esp_spiram_get_size() != 8 * 1024 * 1024 || ESP.getFlashChipSize() != 16 * 1024 * 1024) return;
#ifdef GREENMIND_OTA_TEST_REJECT_BOOT
    if (otaTrialBoot) return;
#endif
    // Confirm only after normal acquisition and ten authenticated, hash-checked uploads.
    if (!otaBootHealthy.load() && directStatus.snapshot().acknowledgements >= 10) {
        if (!otaTrialBoot || esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
            otaBootHealthy.store(true);
            Serial.println("direct_ota_boot_healthy uploads=10");
        }
    }
    static uint32_t lastCheck = 0;
    if (!otaBootHealthy.load() || millis() < 15000 || millis() - lastCheck < 60000) return;
    lastCheck = millis();
    if (otaPendingCommand.length()) {
        const bool success = otaPendingVersion == greenmind::cloud::firmwareVersion;
        if (!otaReport(otaPendingCommand, success ? "booted" : "rollback")) return;
        Preferences prefs; prefs.begin("gmdirectota", false);
        prefs.putString("last_id", otaPendingCommand);
        prefs.remove("pending_id"); prefs.remove("pending_ver"); prefs.end();
        otaPendingCommand = "";
    }
    const auto* partition = esp_ota_get_next_update_partition(nullptr);
    if (!partition || partition->size < 0x300000) return;
    HTTPClient http; WiFiClientSecure tls;
    const String query = String("/check?profile=") + greenmind::directOtaProfile +
        "&current_version=" + greenmind::cloud::firmwareVersion;
    if (!otaHttpBegin(http, tls, query)) return;
    const int result = http.GET();
    if (result == 204) { http.end(); Serial.println("direct_ota_check no_update=1"); return; }
    if (result != 200 || http.getSize() < 0 || http.getSize() > 2048) {
        Serial.printf("direct_ota_check http=%d offered=0\n", result); http.end(); return;
    }
    JsonDocument offer;
    const auto error = deserializeJson(offer, http.getString()); http.end();
    if (error || !offer["size"].is<uint32_t>() || !greenmind::otaOfferValid(
            offer["device_id"] | "", deviceId.c_str(), offer["profile"] | "",
            offer["command_id"] | "", offer["from_version"] | "", greenmind::cloud::firmwareVersion,
            offer["version"] | "", offer["sha256"] | "", offer["size"].as<uint32_t>(), partition->size)) {
        Serial.println("direct_ota_rejected reason=manifest"); return;
    }
    const String command = offer["command_id"].as<String>();
    Preferences prefs; prefs.begin("gmdirectota", true);
    const bool completed = prefs.getString("last_id") == command; prefs.end();
    if (completed) return;
    const String version = offer["version"].as<String>(), digest = offer["sha256"].as<String>();
    otaBusy.store(true); otaPauseRequested.store(true);
    Serial.printf("direct_ota_maintenance command=%s target=%s recording_pause=1\n", command.c_str(), version.c_str());
    const uint32_t pauseStarted = millis();
    while (!otaAcquisitionPaused.load() && millis() - pauseStarted < 3000) vTaskDelay(pdMS_TO_TICKS(10));
    bool drained = otaAcquisitionPaused.load();
    while (drained && directQueue.peek(directPending)) {
        if (millis() - pauseStarted > 30000 || !sendDirect(directPending) ||
            !directQueue.acknowledge(directPending.sequence)) { drained = false; break; }
    }
    const char* failure = nullptr;
    bool verified = drained && directQueue.used() == 0;
    if (verified) verified = otaDownload(command, digest, offer["size"].as<uint32_t>(), failure);
    if (verified) {
        prefs.begin("gmdirectota", false);
        verified = prefs.putString("pending_id", command) && prefs.putString("pending_ver", version);
        prefs.end();
        // Image selection occurs only after hash verification and successful pending-state persistence.
        verified = verified && esp_ota_set_boot_partition(partition) == ESP_OK;
    }
    if (verified) {
        otaReport(command, "staged");
        Serial.println("direct_ota_restart buffered_samples=0 maintenance_gap=1");
        vTaskDelay(pdMS_TO_TICKS(100)); ESP.restart();
    }
    prefs.begin("gmdirectota", false); prefs.remove("pending_id"); prefs.remove("pending_ver");
    // Avoid repeatedly pausing acquisition for the same broken command.
    prefs.putString("last_id", command); prefs.end();
    otaPauseRequested.store(false); otaBusy.store(false);
    Serial.printf("direct_ota_failed command=%s reason=%s boot_unchanged=1 pause_ms=%lu\n",
        command.c_str(), failure ? failure : "drain_or_activate", millis() - pauseStarted);
    otaReport(command, "failed", failure ? failure : "drain_or_activate");
}
#else
static void directOtaTick() {}
#endif
