/**
 * GreenMind OTA Client — ESP32 firmware update via local Raspberry Pi gateway.
 *
 * Flow:
 *   1. GET /api/v1/ota/check?board_type=...&hardware_revision=...&current_version=...
 *   2. If update_available, download binary from /api/v1/ota/download/{id}
 *   3. Stream binary into the inactive OTA partition via Update.h
 *   4. Verify written size matches content-length
 *   5. Reboot into new partition
 *   6. On next boot, report success/failure via POST /api/v1/ota/report
 */

#include "ota_client.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <Update.h>
#include <ArduinoJson.h>
#include <Preferences.h>

// ── Status Reporting ────────────────────────────────────────────────

void GreenMindOTA::reportStatus(const String& gatewayIp, const String& releaseId,
                                const char* status, const char* errorMessage) {
    if (WiFi.status() != WL_CONNECTED) return;

    HTTPClient http;
    String url = "http://" + gatewayIp + "/api/v1/ota/report";
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(10000);

    JsonDocument doc;
    doc["mac_address"] = WiFi.macAddress();
    doc["release_id"] = releaseId;
    doc["status"] = status;
    if (errorMessage) {
        doc["error_message"] = errorMessage;
    }

    String payload;
    serializeJson(doc, payload);

    int httpCode = http.POST(payload);
    Serial.printf("[OTA] Report sent (%s), HTTP: %d\n", status, httpCode);
    http.end();
}

// ── Pending Update Reporting (called on boot) ───────────────────────

static void reportPendingUpdate(const String& gatewayIp) {
    Preferences prefs;
    prefs.begin("ota", true);  // read-only
    String pendingId = prefs.getString("pending_id", "");
    String pendingVer = prefs.getString("pending_ver", "");
    prefs.end();

    if (pendingId.length() == 0) return;

    Serial.printf("[OTA] Reporting pending update result: %s -> %s\n",
                  pendingVer.c_str(), FIRMWARE_VERSION);

    // If the running version matches the pending version, update succeeded
    bool success = (String(FIRMWARE_VERSION) == pendingVer);

    GreenMindOTA::reportStatus(
        gatewayIp, pendingId,
        success ? "success" : "rollback",
        success ? nullptr : "Firmware reverted to previous partition"
    );

    // Clear pending state
    prefs.begin("ota", false);
    prefs.remove("pending_id");
    prefs.remove("pending_ver");
    prefs.end();
}

// ── Update Check ────────────────────────────────────────────────────

void GreenMindOTA::checkAndUpdate(const String& gatewayIp) {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[OTA] WiFi not connected, skipping check");
        return;
    }

    // First, report any pending update from a previous boot
    reportPendingUpdate(gatewayIp);

    HTTPClient http;
    String url = "http://" + gatewayIp + "/api/v1/ota/check"
                 "?board_type=" + String(BOARD_TYPE) +
                 "&hardware_revision=" + String(HARDWARE_REVISION) +
                 "&current_version=" + String(FIRMWARE_VERSION);

    Serial.printf("[OTA] Checking: %s\n", url.c_str());
    http.begin(url);
    http.setTimeout(15000);
    int httpCode = http.GET();

    if (httpCode == 204) {
        Serial.println("[OTA] No updates available");
        http.end();
        return;
    }

    if (httpCode != 200) {
        Serial.printf("[OTA] Check failed, HTTP: %d\n", httpCode);
        http.end();
        return;
    }

    String payload = http.getString();
    http.end();

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, payload);
    if (error) {
        Serial.printf("[OTA] JSON parse error: %s\n", error.c_str());
        return;
    }

    if (!doc["update_available"].as<bool>()) {
        Serial.println("[OTA] No update flagged");
        return;
    }

    String newVersion  = doc["version"].as<String>();
    String downloadUrl = doc["download_url"].as<String>();
    String sha256Hash  = doc["sha256"].as<String>();
    String releaseId   = doc["release_id"] | "unknown";
    bool   mandatory   = doc["mandatory"] | false;

    Serial.printf("[OTA] Update available: %s -> %s (mandatory: %s)\n",
                  FIRMWARE_VERSION, newVersion.c_str(), mandatory ? "yes" : "no");

    // Save pending state in NVS so we can report on next boot
    Preferences prefs;
    prefs.begin("ota", false);
    prefs.putString("pending_id",  releaseId);
    prefs.putString("pending_ver", newVersion);
    prefs.end();

    String fullUrl = "http://" + gatewayIp + downloadUrl;
    bool success = performDownload(fullUrl, sha256Hash);

    if (success) {
        Serial.println("[OTA] Update written. Rebooting into new firmware...");
        delay(500);
        ESP.restart();
    } else {
        Serial.println("[OTA] Update failed");
        reportStatus(gatewayIp, releaseId, "failed", "Download or verification failed");
        // Clear pending state since we won't reboot
        prefs.begin("ota", false);
        prefs.remove("pending_id");
        prefs.remove("pending_ver");
        prefs.end();
    }
}

// ── Binary Download + Flash ─────────────────────────────────────────

bool GreenMindOTA::performDownload(const String& fullUrl, const String& expectedSha256) {
    Serial.printf("[OTA] Downloading: %s\n", fullUrl.c_str());

    HTTPClient http;
    http.begin(fullUrl);
    http.setTimeout(30000);
    int httpCode = http.GET();

    if (httpCode != 200) {
        Serial.printf("[OTA] Download HTTP error: %d\n", httpCode);
        http.end();
        return false;
    }

    int contentLength = http.getSize();
    if (contentLength <= 0) {
        Serial.println("[OTA] Invalid content length");
        http.end();
        return false;
    }

    Serial.printf("[OTA] Firmware size: %d bytes\n", contentLength);

    if (!Update.begin(contentLength)) {
        Serial.printf("[OTA] Not enough space. Error: %d\n", Update.getError());
        http.end();
        return false;
    }

    WiFiClient* client = http.getStreamPtr();
    size_t written = Update.writeStream(*client);
    http.end();

    if (written != (size_t)contentLength) {
        Serial.printf("[OTA] Incomplete write: %u/%d\n", written, contentLength);
        Update.abort();
        return false;
    }

    if (!Update.end()) {
        Serial.printf("[OTA] Finalize error: %d\n", Update.getError());
        return false;
    }

    if (!Update.isFinished()) {
        Serial.println("[OTA] Update not finished");
        return false;
    }

    Serial.printf("[OTA] Written %u bytes, update ready\n", written);
    return true;
}
