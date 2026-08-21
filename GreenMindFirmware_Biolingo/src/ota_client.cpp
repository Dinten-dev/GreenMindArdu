/**
 * GreenMind OTA Client — ESP32 firmware update via local Raspberry Pi gateway.
 *
 * Flow:
 *   1. GET /api/v1/ota/check?board_type=...&hardware_revision=...&current_version=...
 *   2. If update_available, download binary from /api/v1/ota/download/{id}
 *   3. Stream binary into the inactive OTA partition via Update.h
 *   4. Verify content length and the gateway-supplied SHA-256 digest
 *   5. Reboot into new partition
 *   6. On next boot, report success/failure via POST /api/v1/ota/report
 */

#include "ota_client.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <Update.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <mbedtls/sha256.h>

namespace {

constexpr size_t SHA256_DIGEST_SIZE = 32;
constexpr size_t OTA_READ_BUFFER_SIZE = 2048;
constexpr unsigned long OTA_STREAM_IDLE_TIMEOUT_MS = 30000;

int hexNibble(char value) {
    if (value >= '0' && value <= '9')
        return value - '0';
    if (value >= 'a' && value <= 'f')
        return value - 'a' + 10;
    if (value >= 'A' && value <= 'F')
        return value - 'A' + 10;
    return -1;
}

bool parseSha256Hex(const String& value, uint8_t output[SHA256_DIGEST_SIZE]) {
    if (value.length() != SHA256_DIGEST_SIZE * 2)
        return false;

    for (size_t i = 0; i < SHA256_DIGEST_SIZE; ++i) {
        int high = hexNibble(value[i * 2]);
        int low = hexNibble(value[i * 2 + 1]);
        if (high < 0 || low < 0)
            return false;
        output[i] = static_cast<uint8_t>((high << 4) | low);
    }
    return true;
}

bool constantTimeDigestEquals(const uint8_t left[SHA256_DIGEST_SIZE],
                              const uint8_t right[SHA256_DIGEST_SIZE]) {
    uint8_t difference = 0;
    for (size_t i = 0; i < SHA256_DIGEST_SIZE; ++i) {
        difference |= left[i] ^ right[i];
    }
    return difference == 0;
}

} // namespace

// ── Status Reporting ────────────────────────────────────────────────

void GreenMindOTA::reportStatus(const String& gatewayIp, const String& releaseId,
                                const char* status, const char* errorMessage) {
    if (WiFi.status() != WL_CONNECTED)
        return;

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
    prefs.begin("ota", true); // read-only
    String pendingId = prefs.getString("pending_id", "");
    String pendingVer = prefs.getString("pending_ver", "");
    prefs.end();

    if (pendingId.length() == 0)
        return;

    Serial.printf("[OTA] Reporting pending update result: %s -> %s\n", pendingVer.c_str(),
                  FIRMWARE_VERSION);

    // If the running version matches the pending version, update succeeded
    bool success = (String(FIRMWARE_VERSION) == pendingVer);

    GreenMindOTA::reportStatus(gatewayIp, pendingId, success ? "success" : "rollback",
                               success ? nullptr : "Firmware reverted to previous partition");

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
    String url = "http://" + gatewayIp +
                 "/api/v1/ota/check"
                 "?board_type=" +
                 String(BOARD_TYPE) + "&hardware_revision=" + String(HARDWARE_REVISION) +
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

    String newVersion = doc["version"].as<String>();
    String downloadUrl = doc["download_url"].as<String>();
    String sha256Hash = doc["sha256"].as<String>();
    String releaseId = doc["release_id"] | "unknown";
    bool mandatory = doc["mandatory"] | false;

    Serial.printf("[OTA] Update available: %s -> %s (mandatory: %s)\n", FIRMWARE_VERSION,
                  newVersion.c_str(), mandatory ? "yes" : "no");

    // Save pending state in NVS so we can report on next boot
    Preferences prefs;
    prefs.begin("ota", false);
    prefs.putString("pending_id", releaseId);
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
    uint8_t expectedDigest[SHA256_DIGEST_SIZE];
    if (!parseSha256Hex(expectedSha256, expectedDigest)) {
        Serial.println("[OTA] Rejected update: invalid SHA-256 metadata");
        return false;
    }

    Serial.printf("[OTA] Downloading: %s\n", fullUrl.c_str());

    HTTPClient http;
    http.begin(fullUrl);
    http.useHTTP10(true); // Require a finite Content-Length and connection close.
    http.setReuse(false);
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

    size_t firmwareSize = static_cast<size_t>(contentLength);
    size_t availableSketchSpace = ESP.getFreeSketchSpace();
    if (firmwareSize > availableSketchSpace) {
        Serial.printf("[OTA] Firmware too large: %u bytes (available: %u)\n",
                      static_cast<unsigned int>(firmwareSize),
                      static_cast<unsigned int>(availableSketchSpace));
        http.end();
        return false;
    }

    Serial.printf("[OTA] Firmware size: %d bytes\n", contentLength);

    if (!Update.begin(firmwareSize, U_FLASH)) {
        Serial.printf("[OTA] Not enough space. Error: %d\n", Update.getError());
        http.end();
        return false;
    }

    mbedtls_sha256_context shaContext;
    mbedtls_sha256_init(&shaContext);
    if (mbedtls_sha256_starts_ret(&shaContext, 0) != 0) {
        Serial.println("[OTA] Failed to initialize SHA-256 verifier");
        mbedtls_sha256_free(&shaContext);
        Update.abort();
        http.end();
        return false;
    }

    WiFiClient* client = http.getStreamPtr();
    uint8_t readBuffer[OTA_READ_BUFFER_SIZE];
    size_t totalRead = 0;
    unsigned long lastProgressAt = millis();
    const char* streamFailure = nullptr;

    while (totalRead < firmwareSize) {
        int available = client->available();
        if (available <= 0) {
            if (!client->connected()) {
                streamFailure = "connection closed before Content-Length";
                break;
            }
            if (millis() - lastProgressAt > OTA_STREAM_IDLE_TIMEOUT_MS) {
                streamFailure = "stream timed out";
                break;
            }
            delay(1);
            continue;
        }

        size_t remaining = firmwareSize - totalRead;
        size_t toRead = static_cast<size_t>(available);
        if (toRead > sizeof(readBuffer))
            toRead = sizeof(readBuffer);
        if (toRead > remaining)
            toRead = remaining;

        size_t bytesRead = client->readBytes(readBuffer, toRead);
        if (bytesRead == 0) {
            continue;
        }
        if (bytesRead > remaining) {
            streamFailure = "response exceeded Content-Length";
            break;
        }

        size_t bytesWritten = Update.write(readBuffer, bytesRead);
        if (bytesWritten != bytesRead) {
            streamFailure = "flash write was incomplete";
            break;
        }
        if (mbedtls_sha256_update_ret(&shaContext, readBuffer, bytesRead) != 0) {
            streamFailure = "SHA-256 update failed";
            break;
        }

        totalRead += bytesRead;
        lastProgressAt = millis();
    }

    // No byte beyond the declared body size is ever written. Reject an already
    // buffered excess body instead of silently accepting inconsistent metadata.
    if (streamFailure == nullptr && client->available() > 0) {
        streamFailure = "response exceeded Content-Length";
    }

    if (streamFailure != nullptr || totalRead != firmwareSize || !Update.isFinished()) {
        Serial.printf("[OTA] Incomplete/invalid stream: %u/%u bytes (%s)\n",
                      static_cast<unsigned int>(totalRead), static_cast<unsigned int>(firmwareSize),
                      streamFailure != nullptr ? streamFailure : "size mismatch");
        mbedtls_sha256_free(&shaContext);
        Update.abort();
        http.end();
        return false;
    }

    uint8_t actualDigest[SHA256_DIGEST_SIZE];
    if (mbedtls_sha256_finish_ret(&shaContext, actualDigest) != 0) {
        Serial.println("[OTA] Failed to finalize SHA-256 verification");
        mbedtls_sha256_free(&shaContext);
        Update.abort();
        http.end();
        return false;
    }
    mbedtls_sha256_free(&shaContext);
    http.end();

    if (!constantTimeDigestEquals(actualDigest, expectedDigest)) {
        Serial.println("[OTA] SHA-256 mismatch; update aborted");
        Update.abort();
        return false;
    }

    // Finalize the bootable partition only after length and SHA-256 verification.
    if (!Update.end()) {
        Serial.printf("[OTA] Finalize error: %d\n", Update.getError());
        return false;
    }

    if (!Update.isFinished()) {
        Serial.println("[OTA] Update not finished");
        return false;
    }

    Serial.printf("[OTA] Verified and written %u bytes, update ready\n", totalRead);
    return true;
}
