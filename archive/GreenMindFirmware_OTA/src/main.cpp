/**
 * GreenMind ESP32 Sensor Firmware with OTA Support
 *
 * Combines:
 *   - Captive portal setup (WiFi + pairing code provisioning)
 *   - 380 Hz ADC sampling + batch POST to gateway
 *   - OTA firmware updates via local Raspberry Pi gateway
 *
 * OTA check runs:
 *   - Once on boot (after WiFi + gateway connect)
 *   - Every 60 minutes during runtime
 *
 * Config persisted in NVS (Preferences).
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <HTTPClient.h>
#include <WiFiUdp.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include "ota_client.h"

// ── Pin Configuration ─────────────────────────
static const int ADC_PIN = 34;

// ── Globals ───────────────────────────────────
Preferences prefs;
WebServer   server(80);
DNSServer   dnsServer;
WiFiUDP     udp;

String wifiSSID;
String wifiPass;
String pairingCode;
String gatewayIP;
String macAddress;

bool isProvisioned = false;

// ── Forward declarations ──────────────────────
void startSetupMode();
void startRuntimeMode();
String getMacAddress();
bool discoverGateway();
bool registerSensor();
void streamReadings();
void sendBatch();

// ── OTA timing ────────────────────────────────
static unsigned long lastOtaCheck = 0;
static const unsigned long OTA_CHECK_INTERVAL = 3600000; // 1 hour

// ── HTML Captive Portal ───────────────────────
static const char HTML_SETUP[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html><head><meta name="viewport" content="width=device-width, initial-scale=1">
<title>Sensor Setup</title>
<style>
body{font-family:-apple-system,sans-serif;background:#f0fdf4;margin:0;padding:20px;color:#1f2937}
.card{background:#fff;padding:25px;border-radius:20px;max-width:400px;margin:40px auto;box-shadow:0 4px 6px rgba(0,0,0,.05)}
h2{color:#10b981;margin-top:0}
label{font-size:13px;font-weight:700;color:#6b7280;display:block;margin-bottom:5px}
input{width:100%;padding:12px;margin-bottom:15px;border:1px solid #e5e7eb;border-radius:10px;box-sizing:border-box;font-size:16px}
button{width:100%;padding:14px;background:#10b981;color:#fff;border:none;border-radius:12px;font-size:16px;font-weight:700;cursor:pointer}
.ver{text-align:center;font-size:11px;color:#9ca3af;margin-top:15px}
</style></head><body>
<div class="card">
<h2>GreenMind Sensor</h2>
<p>Verbinde den Sensor mit dem WLAN.</p>
<form method="POST" action="/provision">
<label>WLAN Name (SSID)</label>
<input type="text" name="ssid" required placeholder="Mein Heimnetzwerk">
<label>WLAN Passwort</label>
<input type="password" name="password" required>
<label>Pairing Code (vom Dashboard)</label>
<input type="text" name="code" required placeholder="ABC123"
       pattern="[a-zA-Z0-9]+" maxlength="6" style="text-transform:uppercase">
<button type="submit">Jetzt Speichern</button>
</form>
<p class="ver">Firmware v)rawliteral" FIRMWARE_VERSION R"rawliteral( · OTA enabled</p>
</div></body></html>
)rawliteral";

static const char HTML_SUCCESS[] PROGMEM =
  "<!DOCTYPE html><html><body style='font-family:sans-serif;text-align:center;padding:60px'>"
  "<h2 style='color:#10b981'>&#10003; Erfolgreich gespeichert!</h2>"
  "<p>Sensor startet neu&hellip;</p></body></html>";

// ══════════════════════════════════════════════
//  SETUP & LOOP
// ══════════════════════════════════════════════

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.printf("\n[GreenMind] Booting v%s (OTA enabled)\n", FIRMWARE_VERSION);

    analogReadResolution(12);
    analogSetAttenuation(ADC_11db);

    // Get STA MAC
    WiFi.mode(WIFI_STA);
    delay(100);
    macAddress = getMacAddress();
    WiFi.mode(WIFI_MODE_NULL);

    // Load config
    prefs.begin("gm", false);
    wifiSSID    = prefs.getString("ssid", "");
    wifiPass    = prefs.getString("pass", "");
    pairingCode = prefs.getString("code", "");
    gatewayIP   = "192.168.1.91"; // Hardcoded for test
    prefs.end();

    isProvisioned = (wifiSSID.length() > 0 && wifiPass.length() > 0);

    Serial.printf("[GreenMind] MAC: %s  Provisioned: %s\n",
                  macAddress.c_str(), isProvisioned ? "yes" : "no");

    if (isProvisioned) {
        startRuntimeMode();
    } else {
        startSetupMode();
    }
}

void loop() {
    if (!isProvisioned) {
        dnsServer.processNextRequest();
        server.handleClient();
    } else {
        streamReadings();

        // Periodic OTA check
        if (millis() - lastOtaCheck > OTA_CHECK_INTERVAL) {
            lastOtaCheck = millis();
            Serial.println("[GreenMind] Periodic OTA check...");
            GreenMindOTA::checkAndUpdate(gatewayIP);
        }
    }
}

// ══════════════════════════════════════════════
//  HELPERS
// ══════════════════════════════════════════════

String getMacAddress() {
    uint8_t mac[6];
    WiFi.macAddress(mac);
    char buf[18];
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return String(buf);
}

void saveConfig() {
    prefs.begin("gm", false);
    prefs.putString("ssid", wifiSSID);
    prefs.putString("pass", wifiPass);
    prefs.putString("code", pairingCode);
    prefs.putString("gwip", gatewayIP);
    prefs.end();
}

void factoryReset() {
    Serial.println("[GreenMind] Factory reset!");
    prefs.begin("gm", false);
    prefs.clear();
    prefs.end();
    // Clear OTA state too
    prefs.begin("ota", false);
    prefs.clear();
    prefs.end();
    delay(500);
    ESP.restart();
}

// ══════════════════════════════════════════════
//  SETUP MODE (Captive Portal)
// ══════════════════════════════════════════════

void handleRoot() {
    server.send_P(200, "text/html", HTML_SETUP);
}

void handleProvision() {
    wifiSSID    = server.arg("ssid");
    wifiPass    = server.arg("password");
    pairingCode = server.arg("code");
    pairingCode.toUpperCase();
    pairingCode.trim();

    Serial.printf("[GreenMind] Provisioned: SSID=%s  Code=%s\n",
                  wifiSSID.c_str(), pairingCode.c_str());
    saveConfig();

    server.send_P(200, "text/html", HTML_SUCCESS);
    delay(1500);
    ESP.restart();
}

void handleCaptiveRedirect() {
    server.sendHeader("Location", "http://192.168.4.1/");
    server.send(302, "text/plain", "");
}

void startSetupMode() {
    Serial.println("[GreenMind] Starting Setup Mode (AP)");

    String suffix = macAddress.substring(macAddress.length() - 5);
    suffix.replace(":", "");
    String apName = "GreenMind-Sensor-" + suffix;

    WiFi.mode(WIFI_AP);
    WiFi.softAP(apName.c_str());
    delay(200);

    Serial.printf("[GreenMind] AP: %s  IP: %s\n",
                  apName.c_str(), WiFi.softAPIP().toString().c_str());

    dnsServer.start(53, "*", WiFi.softAPIP());

    server.on("/",            HTTP_GET,  handleRoot);
    server.on("/setup",       HTTP_GET,  handleRoot);
    server.on("/provision",   HTTP_POST, handleProvision);
    server.on("/provision",   HTTP_DELETE, []() {
        server.send(200); factoryReset();
    });

    server.on("/hotspot-detect.html", HTTP_GET, handleCaptiveRedirect);
    server.on("/generate_204",        HTTP_GET, handleCaptiveRedirect);
    server.on("/gen_204",             HTTP_GET, handleCaptiveRedirect);
    server.on("/connecttest.txt",     HTTP_GET, handleCaptiveRedirect);
    server.on("/ncsi.txt",            HTTP_GET, handleCaptiveRedirect);
    server.on("/redirect",            HTTP_GET, handleCaptiveRedirect);
    server.on("/canonical.html",      HTTP_GET, handleCaptiveRedirect);
    server.onNotFound(handleCaptiveRedirect);

    server.begin();
}

// ══════════════════════════════════════════════
//  RUNTIME MODE
// ══════════════════════════════════════════════

void startRuntimeMode() {
    Serial.printf("[GreenMind] Connecting to WiFi: %s\n", wifiSSID.c_str());

    WiFi.mode(WIFI_STA);
    WiFi.begin(wifiSSID.c_str(), wifiPass.c_str());

    int retries = 30;
    while (WiFi.status() != WL_CONNECTED && retries-- > 0) {
        delay(1000);
        Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[GreenMind] WiFi failed! Rebooting...");
        delay(3000);
        ESP.restart();
    }

    Serial.printf("[GreenMind] WiFi OK  IP: %s\n", WiFi.localIP().toString().c_str());

    // Discover gateway
    if (!discoverGateway()) {
        Serial.println("[GreenMind] Gateway not found! Rebooting...");
        delay(3000);
        ESP.restart();
    }

    Serial.printf("[GreenMind] Gateway at %s\n", gatewayIP.c_str());

    // ── OTA check on boot ──
    Serial.println("[GreenMind] Boot OTA check...");
    GreenMindOTA::checkAndUpdate(gatewayIP);
    lastOtaCheck = millis();

    // Register sensor if pairing code exists
    if (pairingCode.length() > 0) {
        if (registerSensor()) {
            Serial.println("[GreenMind] Sensor registered");
        } else {
            Serial.println("[GreenMind] Registration failed, continuing");
        }
        pairingCode = "";
        saveConfig();
    }

    // Health + reset endpoints
    server.on("/", HTTP_DELETE, []() {
        server.send(200, "text/plain", "OK");
        delay(500);
        factoryReset();
    });
    server.on("/health", HTTP_GET, []() {
        String json = "{\"status\":\"ok\",\"version\":\"" + String(FIRMWARE_VERSION) + "\",\"ota\":true}";
        server.send(200, "application/json", json);
    });
    server.begin();

    Serial.printf("[GreenMind] Streaming started (v%s, OTA enabled)\n", FIRMWARE_VERSION);
}

// ── Gateway Discovery ─────────────────────────

bool checkGatewayHealth(const String& ip) {
    HTTPClient http;
    String url = "http://" + ip + "/api/v1/health";
    http.begin(url);
    http.setTimeout(2000);
    int code = http.GET();
    String body = "";
    if (code == 200) {
        body = http.getString();
    }
    http.end();
    return (code == 200 && body.indexOf("hardware_id") >= 0);
}

bool discoverGateway() {
    // 1) Cached IP
    if (gatewayIP.length() > 0) {
        Serial.printf("[GreenMind] Trying cached: %s\n", gatewayIP.c_str());
        if (checkGatewayHealth(gatewayIP)) return true;
        gatewayIP = "";
    }

    // 2) UDP broadcast
    Serial.println("[GreenMind] UDP discovery...");
    WiFiUDP disc;
    disc.begin(50001);
    for (int attempt = 0; attempt < 5; attempt++) {
        disc.beginPacket("255.255.255.255", 50000);
        disc.print("DISCOVER_GREENMIND_GATEWAY");
        disc.endPacket();

        unsigned long start = millis();
        while (millis() - start < 2000) {
            int len = disc.parsePacket();
            if (len > 0) {
                char buf[64];
                int n = disc.read(buf, sizeof(buf) - 1);
                buf[n] = '\0';
                String msg(buf);
                if (msg.startsWith("GATEWAY_IP:")) {
                    String sourceIP = disc.remoteIP().toString();
                    String payloadIP = msg.substring(11);
                    disc.stop();
                    if (checkGatewayHealth(sourceIP)) {
                        gatewayIP = sourceIP;
                    } else if (checkGatewayHealth(payloadIP)) {
                        gatewayIP = payloadIP;
                    } else {
                        break;
                    }
                    saveConfig();
                    return true;
                }
            }
            delay(50);
        }
    }
    disc.stop();

    // 3) Subnet scan
    Serial.println("[GreenMind] Subnet scan...");
    IPAddress myIP = WiFi.localIP();
    for (int host = 1; host < 255; host++) {
        IPAddress candidate(myIP[0], myIP[1], myIP[2], host);
        if (candidate == myIP) continue;
        yield();

        WiFiClient client;
        client.setTimeout(100);
        if (client.connect(candidate, 80)) {
            client.print("GET /api/v1/health HTTP/1.0\r\nHost: gm\r\n\r\n");
            unsigned long t = millis();
            while (!client.available() && millis() - t < 300) { delay(10); yield(); }
            String resp = client.readString();
            client.stop();
            if (resp.indexOf("hardware_id") >= 0) {
                gatewayIP = candidate.toString();
                saveConfig();
                return true;
            }
        }
        if (host % 20 == 0) Serial.printf("[GreenMind] Scanned %d/254\n", host);
    }
    return false;
}

// ── Sensor Registration ───────────────────────

bool registerSensor() {
    JsonDocument doc;
    doc["mac_address"] = macAddress;
    doc["code"]        = pairingCode;

    String body;
    serializeJson(doc, body);

    HTTPClient http;
    String url = "http://" + gatewayIP + "/api/v1/sensors/register";
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(10000);

    int code = http.POST(body);
    http.end();
    return (code == 200 || code == 201);
}

// ── Signal Filtering ──────────────────────────

static const int FILTER_SIZE = 3;
static float filterBuffer[FILTER_SIZE];
static int filterIndex = 0;

float applyFilter(float newValue) {
    filterBuffer[filterIndex] = newValue;
    filterIndex = (filterIndex + 1) % FILTER_SIZE;
    float sum = 0.0;
    for (int i = 0; i < FILTER_SIZE; i++) sum += filterBuffer[i];
    return sum / FILTER_SIZE;
}

// ── High-Frequency Streaming ──────────────────

static const int SAMPLE_RATE = 380;
static const int SAMPLE_INTERVAL_US = 1000000 / SAMPLE_RATE;
static const int BATCH_SIZE = 380;
static const float ADC_VOLTAGE_REF = 3.3f;

static float sampleBuffer[BATCH_SIZE];
static int bufferIndex = 0;
static unsigned long lastSampleTime = 0;
static unsigned long lastWifiCheck = 0;

void streamReadings() {
    server.handleClient();

    if (millis() - lastWifiCheck > 5000) {
        lastWifiCheck = millis();
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("[GreenMind] WiFi lost...");
            WiFi.reconnect();
            int retries = 15;
            while (WiFi.status() != WL_CONNECTED && retries-- > 0) delay(1000);
            if (WiFi.status() != WL_CONNECTED) {
                ESP.restart();
            }
        }
    }

    unsigned long now = micros();
    if (now - lastSampleTime >= SAMPLE_INTERVAL_US) {
        lastSampleTime = now;

        int raw = analogRead(ADC_PIN);
        float mv = (raw / 4095.0f) * ADC_VOLTAGE_REF * 1000.0f;
        float filtered = applyFilter(mv);

        sampleBuffer[bufferIndex++] = filtered;

        if (bufferIndex >= BATCH_SIZE) {
            sendBatch();
            bufferIndex = 0;
        }
    }
}

void sendBatch() {
    static int errorCount = 0;

    JsonDocument doc;
    doc["mac_address"] = macAddress;
    doc["sample_rate"] = SAMPLE_RATE;
    JsonArray readings = doc["readings"].to<JsonArray>();

    for (int i = 0; i < BATCH_SIZE; i++) {
        JsonObject r = readings.add<JsonObject>();
        r["kind"]  = "bio_signal";
        r["value"] = serialized(String(sampleBuffer[i], 1));
        r["unit"]  = "mV";
    }

    String payload;
    serializeJson(doc, payload);

    HTTPClient http;
    String url = "http://" + gatewayIP + "/api/v1/ingest";
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(5000);

    int code = http.POST(payload);
    http.end();

    if (code == 200 || code == 201) {
        errorCount = 0;
    } else {
        errorCount++;
        Serial.printf("[GreenMind] Stream error: HTTP %d (count: %d)\n", code, errorCount);
        if (errorCount > 20) ESP.restart();
    }
}
