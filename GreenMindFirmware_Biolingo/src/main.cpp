/**
 * GreenMind Biolingo v22 — ESP32-S3 Sensor Firmware
 *
 * Hardware: ESP32-S3-WROOM-1 + AD8232 + SSD1306 OLED (128x64 I2C)
 * Board:    Biolingo v22 (KiCad Rev v22)
 *
 * Features:
 *   - Captive portal setup (WiFi + pairing code provisioning)
 *   - 380 Hz ADC sampling with 3-sample moving average
 *   - AD8232 lead-off detection + artifact flagging
 *   - Batch POST to gateway (/api/v1/ingest, 380 samples/s)
 *   - Gateway discovery: cached IP → UDP broadcast → subnet scan
 *   - OTA firmware updates via local Raspberry Pi gateway
 *   - SSD1306 OLED status display (MAC, WiFi, GW, TX, lead-off)
 *   - Factory reset via HTTP DELETE from gateway
 *
 * Pin Mapping (Biolingo v22 schematic):
 *   IO4  → ADC1_CH3  (AD8232 filtered output)
 *   IO5  → LOD+      (AD8232 lead-off detection plus)
 *   IO6  → LOD-      (AD8232 lead-off detection minus)
 *   IO12 → SCL       (SSD1306 OLED I2C clock)
 *   IO13 → SDA       (SSD1306 OLED I2C data)
 *   IO0  → SW2       (Boot button, active low)
 *   EN   → SW1       (Hardware reset)
 *
 * Config persisted in NVS (Preferences).
 * Gateway API:
 *   POST   /api/v1/ingest            (batch readings)
 *   POST   /api/v1/sensors/register  (pairing)
 *   DELETE /                          (factory reset from gateway)
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
#include "display.h"

// ── Pin Configuration (Biolingo v22) ──────────
static const int ADC_PIN      = 4;    // IO4, ADC1_CH3
static const int LO_PLUS_PIN  = 5;    // IO5, AD8232 LOD+
static const int LO_MINUS_PIN = 6;    // IO6, AD8232 LOD-

// ── ADC & Sampling ───────────────────────────
static const int   SAMPLE_RATE        = 380;
static const int   SAMPLE_INTERVAL_US = 1000000 / SAMPLE_RATE;  // ~2632 µs
static const int   BATCH_SIZE         = 380;                     // 1 second
static const float ADC_VOLTAGE_REF    = 3.3f;

// ── AD8232 Artifact Detection Thresholds ──────
static const float RAIL_HIGH_THRESHOLD     = 3200.0f;  // mV
static const float RAIL_LOW_THRESHOLD      = 100.0f;   // mV
static const float JUMP_THRESHOLD          = 500.0f;   // mV
static const int   RECOVERY_SAMPLES_COUNT  = 38;       // ~100 ms recovery window

// ── Artifact Flag Bitmask ─────────────────────
#define FLAG_VALID        0
#define FLAG_LEAD_OFF     1
#define FLAG_RAIL_HIGH    2
#define FLAG_RAIL_LOW     4
#define FLAG_JUMP         8
#define FLAG_RECOVERY    16

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

// ── Sampling Buffers ──────────────────────────
static float   sampleBuffer[BATCH_SIZE];
static uint8_t lpBuffer[BATCH_SIZE];
static uint8_t lmBuffer[BATCH_SIZE];
static uint8_t flagBuffer[BATCH_SIZE];

static int           bufferIndex      = 0;
static unsigned long lastSampleTime   = 0;
static unsigned long lastWifiCheck    = 0;
static float         lastValidValue   = -1.0f;
static int           recoveryCounter  = 0;

// ── Signal Filter ─────────────────────────────
static const int FILTER_SIZE = 3;
static float     filterBuf[FILTER_SIZE];
static int       filterIdx = 0;

// ── OTA Timing ────────────────────────────────
static unsigned long lastOtaCheck = 0;
static const unsigned long OTA_CHECK_INTERVAL = 3600000;  // 1 hour

// ── Streaming State (for display) ─────────────
static bool lastSendOk    = false;
static int  streamErrors  = 0;
static bool currentLeadOff = false;

// ── Forward Declarations ──────────────────────
void startSetupMode();
void startRuntimeMode();
String getMacAddress();
bool discoverGateway();
bool registerSensor();
void streamReadings();
void sendBatch();

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
<p class="ver">Biolingo v22 · FW v)rawliteral" FIRMWARE_VERSION R"rawliteral( · OTA</p>
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
    Serial.printf("\n[Biolingo] Booting v%s (OTA enabled)\n", FIRMWARE_VERSION);

    // Pin modes
    pinMode(ADC_PIN, INPUT);
    pinMode(LO_PLUS_PIN, INPUT);
    pinMode(LO_MINUS_PIN, INPUT);

    analogReadResolution(12);
    analogSetAttenuation(ADC_11db);

    // Init OLED display
    Display::init();

    // Get STA MAC
    WiFi.mode(WIFI_STA);
    delay(100);
    macAddress = getMacAddress();
    WiFi.mode(WIFI_MODE_NULL);

    // Show boot screen
    Display::showBoot(macAddress);

    // Load config from NVS
    prefs.begin("gm", false);
    wifiSSID    = prefs.getString("ssid", "");
    wifiPass    = prefs.getString("pass", "");
    pairingCode = prefs.getString("code", "");
    gatewayIP   = prefs.getString("gwip", "");
    prefs.end();

    isProvisioned = (wifiSSID.length() > 0 && wifiPass.length() > 0);

    Serial.printf("[Biolingo] MAC: %s  Provisioned: %s\n",
                  macAddress.c_str(), isProvisioned ? "yes" : "no");
    Serial.printf("[Biolingo] Cached GW: '%s'  Code: '%s'\n",
                  gatewayIP.c_str(), pairingCode.c_str());

    delay(1500);  // Let user see boot screen

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
            Serial.println("[Biolingo] Periodic OTA check...");
            Display::showOtaCheck();
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
    Serial.println("[Biolingo] Config saved to NVS");
}

void factoryReset() {
    Serial.println("[Biolingo] Factory reset!");
    Display::showError("Factory Reset", "Wiping config...");

    prefs.begin("gm", false);
    prefs.clear();
    prefs.end();
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

    Serial.printf("[Biolingo] Provisioned: SSID=%s  Code=%s\n",
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
    Serial.println("[Biolingo] Starting Setup Mode (AP)");

    // Build AP name from last 4 hex chars of MAC
    String suffix = macAddress.substring(macAddress.length() - 5);
    suffix.replace(":", "");
    String apName = "GreenMind-Sensor-" + suffix;

    WiFi.mode(WIFI_AP);
    WiFi.softAP(apName.c_str());
    delay(200);

    Serial.printf("[Biolingo] AP: %s  IP: %s\n",
                  apName.c_str(), WiFi.softAPIP().toString().c_str());

    // Display setup screen
    Display::showSetup(apName);

    // DNS: redirect ALL queries to our IP (captive portal trigger)
    dnsServer.start(53, "*", WiFi.softAPIP());

    // HTTP routes
    server.on("/",            HTTP_GET,  handleRoot);
    server.on("/setup",       HTTP_GET,  handleRoot);
    server.on("/provision",   HTTP_POST, handleProvision);
    server.on("/provision",   HTTP_DELETE, []() {
        server.send(200); factoryReset();
    });

    // Captive portal detection endpoints
    server.on("/hotspot-detect.html", HTTP_GET, handleCaptiveRedirect);
    server.on("/generate_204",        HTTP_GET, handleCaptiveRedirect);
    server.on("/gen_204",             HTTP_GET, handleCaptiveRedirect);
    server.on("/connecttest.txt",     HTTP_GET, handleCaptiveRedirect);
    server.on("/ncsi.txt",            HTTP_GET, handleCaptiveRedirect);
    server.on("/redirect",            HTTP_GET, handleCaptiveRedirect);
    server.on("/canonical.html",      HTTP_GET, handleCaptiveRedirect);
    server.onNotFound(handleCaptiveRedirect);

    server.begin();
    Serial.println("[Biolingo] Setup server ready");
}

// ══════════════════════════════════════════════
//  RUNTIME MODE
// ══════════════════════════════════════════════

void startRuntimeMode() {
    Serial.printf("[Biolingo] Connecting to WiFi: %s\n", wifiSSID.c_str());
    Display::showConnecting(wifiSSID);

    WiFi.mode(WIFI_STA);
    WiFi.begin(wifiSSID.c_str(), wifiPass.c_str());

    int retries = 30;
    while (WiFi.status() != WL_CONNECTED && retries-- > 0) {
        delay(1000);
        Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[Biolingo] WiFi failed! Rebooting...");
        Display::showError("WiFi failed!", wifiSSID);
        delay(3000);
        ESP.restart();
    }

    Serial.printf("[Biolingo] WiFi OK  IP: %s\n", WiFi.localIP().toString().c_str());

    // Discover gateway
    if (!discoverGateway()) {
        Serial.println("[Biolingo] Gateway not found! Rebooting...");
        Display::showError("Gateway not", "found!");
        delay(3000);
        ESP.restart();
    }

    Serial.printf("[Biolingo] Gateway at %s\n", gatewayIP.c_str());

    // OTA check on boot
    Serial.println("[Biolingo] Boot OTA check...");
    Display::showOtaCheck();
    GreenMindOTA::checkAndUpdate(gatewayIP);
    lastOtaCheck = millis();

    // Register sensor if pairing code exists
    if (pairingCode.length() > 0) {
        Display::showRegistering();
        if (registerSensor()) {
            Serial.println("[Biolingo] Sensor registered");
        } else {
            Serial.println("[Biolingo] Registration failed, continuing");
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
        String json = "{\"status\":\"ok\",\"version\":\"" + String(FIRMWARE_VERSION)
                     + "\",\"board\":\"BIOLINGO_V22\",\"ota\":true}";
        server.send(200, "application/json", json);
    });
    server.begin();

    // Initial display
    Display::showStreaming(macAddress, true, true, true, 0, false, 0.0f);

    Serial.printf("[Biolingo] Streaming started (v%s, OTA, AD8232)\n", FIRMWARE_VERSION);
}

// ── Gateway Discovery ─────────────────────────
// Strategy: cached IP → UDP broadcast → subnet scan

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
        Serial.printf("[Biolingo] Trying cached: %s\n", gatewayIP.c_str());
        Display::showSearchGW("Cached");
        if (checkGatewayHealth(gatewayIP)) return true;
        gatewayIP = "";
    }

    // 2) UDP broadcast
    Serial.println("[Biolingo] UDP discovery...");
    Display::showSearchGW("UDP Broadcast");

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
                    Serial.printf("[Biolingo] UDP reply: payload=%s source=%s\n",
                                  payloadIP.c_str(), sourceIP.c_str());
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
    Serial.println("[Biolingo] Subnet scan...");
    Display::showSearchGW("Subnet Scan");

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
                Serial.printf("[Biolingo] Found via scan: %s\n", gatewayIP.c_str());
                saveConfig();
                return true;
            }
        }
        if (host % 20 == 0) Serial.printf("[Biolingo] Scanned %d/254\n", host);
    }
    return false;
}

// ── Sensor Registration ───────────────────────

bool registerSensor() {
    Serial.printf("[Biolingo] Registering with code: %s\n", pairingCode.c_str());

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
// 3-sample moving average for noise reduction

float applyFilter(float newValue) {
    filterBuf[filterIdx] = newValue;
    filterIdx = (filterIdx + 1) % FILTER_SIZE;
    float sum = 0.0f;
    for (int i = 0; i < FILTER_SIZE; i++) sum += filterBuf[i];
    return sum / FILTER_SIZE;
}

// ── High-Frequency Data Streaming ─────────────
// 380 Hz sampling with AD8232 artifact detection

void streamReadings() {
    // Handle incoming requests (DELETE, health) non-blocking
    server.handleClient();

    // WiFi watchdog (every 5 seconds)
    if (millis() - lastWifiCheck > 5000) {
        lastWifiCheck = millis();
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("[Biolingo] WiFi lost, reconnecting...");
            Display::showError("WiFi lost!", "Reconnecting...");
            WiFi.reconnect();
            int retries = 15;
            while (WiFi.status() != WL_CONNECTED && retries-- > 0) delay(1000);
            if (WiFi.status() != WL_CONNECTED) {
                Serial.println("[Biolingo] WiFi reconnect failed, rebooting");
                ESP.restart();
            }
        }
    }

    // Timer-based sampling at 380 Hz
    unsigned long now = micros();
    if (now - lastSampleTime >= (unsigned long)SAMPLE_INTERVAL_US) {
        lastSampleTime += SAMPLE_INTERVAL_US;  // Exact timing compensation

        // 1. Read raw values synchronously
        int rawAdc = analogRead(ADC_PIN);
        uint8_t lp = digitalRead(LO_PLUS_PIN);
        uint8_t lm = digitalRead(LO_MINUS_PIN);

        // 2. Convert to millivolts and apply filter
        float mv = (rawAdc / 4095.0f) * ADC_VOLTAGE_REF * 1000.0f;
        float filteredMv = applyFilter(mv);

        // 3. Artifact detection
        uint8_t flags = FLAG_VALID;
        bool isInvalid = false;

        // Lead-off detection (AD8232 LOD+ / LOD-)
        if (lp == HIGH || lm == HIGH) {
            flags |= FLAG_LEAD_OFF;
            isInvalid = true;
            currentLeadOff = true;
        } else {
            currentLeadOff = false;
        }

        // Rail detection
        if (filteredMv > RAIL_HIGH_THRESHOLD) {
            flags |= FLAG_RAIL_HIGH;
            isInvalid = true;
        }
        if (filteredMv < RAIL_LOW_THRESHOLD) {
            flags |= FLAG_RAIL_LOW;
            isInvalid = true;
        }

        // Jump artifact detection
        if (!isInvalid && lastValidValue >= 0) {
            if (fabs(filteredMv - lastValidValue) > JUMP_THRESHOLD) {
                flags |= FLAG_JUMP;
                isInvalid = true;
            }
        }

        // Recovery state machine
        if (isInvalid) {
            recoveryCounter = RECOVERY_SAMPLES_COUNT;
        } else if (recoveryCounter > 0) {
            flags |= FLAG_RECOVERY;
            recoveryCounter--;
            isInvalid = true;
        }

        // Update baseline only when perfectly valid
        if (!isInvalid) {
            lastValidValue = filteredMv;
        }

        // 4. Store in batch buffer
        sampleBuffer[bufferIndex] = filteredMv;
        lpBuffer[bufferIndex]     = lp;
        lmBuffer[bufferIndex]     = lm;
        flagBuffer[bufferIndex]   = flags;
        bufferIndex++;

        // 5. Send batch when full (380 samples = 1 second)
        if (bufferIndex >= BATCH_SIZE) {
            // Calculate batch mean for display
            float batchMean = 0.0f;
            for (int i = 0; i < BATCH_SIZE; i++) batchMean += sampleBuffer[i];
            batchMean /= BATCH_SIZE;

            sendBatch();
            bufferIndex = 0;

            // Update display after each batch (once per second)
            bool wifiOk = (WiFi.status() == WL_CONNECTED);
            Display::showStreaming(macAddress, wifiOk, true,
                                  lastSendOk, streamErrors, currentLeadOff,
                                  batchMean);
        }
    }
}

void sendBatch() {
    // Build JSON payload (standard production format for /api/v1/ingest)
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

    // POST to gateway
    HTTPClient http;
    String url = "http://" + gatewayIP + "/api/v1/ingest";
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(5000);

    int code = http.POST(payload);
    http.end();

    if (code == 200 || code == 201) {
        streamErrors = 0;
        lastSendOk = true;
        Serial.printf("[Biolingo] Sent %d samples @ %d Hz [OK]\n", BATCH_SIZE, SAMPLE_RATE);
    } else {
        streamErrors++;
        lastSendOk = false;
        Serial.printf("[Biolingo] Stream error: HTTP %d (count: %d)\n", code, streamErrors);
        if (streamErrors > 20) {
            Serial.println("[Biolingo] Too many errors, rebooting");
            Display::showError("Too many TX", "errors! Reboot");
            delay(2000);
            ESP.restart();
        }
    }
}
