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
#include <WiFiProv.h>
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
WiFiUDP     udp;

String wifiSSID;
String wifiPass;
String pairingCode;
String gatewayIP;
String macAddress;

bool isProvisioned = false;

// ── Sampling Buffers & Queues ─────────────────
struct SensorBatch {
    float   sampleBuffer[BATCH_SIZE];
    uint8_t lpBuffer[BATCH_SIZE];
    uint8_t lmBuffer[BATCH_SIZE];
    uint8_t flagBuffer[BATCH_SIZE];
};

static SensorBatch   buffers[2];
static int           currentBuffer    = 0;
static int           bufferIndex      = 0;
static QueueHandle_t uploadQueue      = NULL;
static TaskHandle_t  uploadTaskHandle = NULL;

static unsigned long lastSampleTime   = 0;
static unsigned long lastWifiCheck    = 0;
static unsigned long setupModeStartTime = 0;
static float         lastValidValue   = -1.0f;
static int           recoveryCounter  = 0;

// ── Signal Filter ─────────────────────────────
static const float LP_ALPHA      = 0.25f;   // Gloor ~20 Hz IIR lowpass (DC-preserving)
static float       lpFilterState = 0.0f;    // first-stage 50 Hz attenuation
static bool        lpFilterInit  = false;

// 50 Hz mains notch (biquad, second stage, applied after the lowpass)
static const float NOTCH_FREQ = 50.0f;      // mains frequency (Hz)
static const float NOTCH_Q    = 30.0f;      // notch sharpness (higher = narrower)
static float n_b0, n_b1, n_b2, n_a1, n_a2;  // normalized biquad coefficients
static float n_z1 = 0.0f, n_z2 = 0.0f;      // transposed-DF2 state
static bool  notchReady = false;

// ── OTA Timing ────────────────────────────────
static unsigned long lastOtaCheck = 0;
static const unsigned long OTA_CHECK_INTERVAL = 3600000;  // 1 hour

// ── Streaming State (for display) ─────────────
static volatile bool lastSendOk    = false;
static volatile int  streamErrors  = 0;
static bool currentLeadOff = false;

// ── Forward Declarations ──────────────────────
void startSetupMode();
void startRuntimeMode();
String getMacAddress();
void saveConfig();
bool discoverGateway();
bool registerSensor();
void streamReadings();
void sendBatch(SensorBatch* batch);
void uploadTaskCode(void *pvParameters);

// ── Helpers für BLE Provisioning ──────────────
String generatePairingCode() {
    String code = "";
    for (int i = 0; i < 6; i++) {
        int r = random(0, 36);
        if (r < 10) code += String(r);
        else code += String((char)('A' + r - 10));
    }
    return code;
}

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

    // Auto-provisioning: force WiFi credentials from build flags
    #ifdef PROVISION_SSID
    if (wifiSSID != PROVISION_SSID || wifiPass != PROVISION_PASS) {
        Serial.println("[Biolingo] Auto-provisioning WiFi from build flags");
        wifiSSID = PROVISION_SSID;
        wifiPass = PROVISION_PASS;
        gatewayIP = "";  // Clear cached GW so discovery runs fresh
        pairingCode = "";
        saveConfig();
    }
    #endif

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
        if (WiFi.status() == WL_CONNECTED && wifiSSID.length() == 0) {
            wifiSSID = WiFi.SSID();
            wifiPass = WiFi.psk();
            Serial.printf("[Biolingo] Provisioned successfully! SSID: %s\n", wifiSSID.c_str());
            saveConfig();
            delay(1000);
            ESP.restart();
        }
        if (millis() - setupModeStartTime > 300000) {
            Serial.println("[Biolingo] 5 min timeout. Rebooting...");
            ESP.restart();
        }
        delay(100);
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
//  SETUP MODE (BLE Provisioning)
// ══════════════════════════════════════════════

void startSetupMode() {
    Serial.println("[Biolingo] Starting Setup Mode (BLE Provisioning)");

    String suffix = macAddress.substring(macAddress.length() - 5);
    suffix.replace(":", "");
    String bleName = "GM-" + suffix;

    String generatedCode = generatePairingCode();
    Serial.printf("[Biolingo] BLE Name: %s  Code: %s\n", bleName.c_str(), generatedCode.c_str());

    Display::showBleProvisioning(bleName, generatedCode);
    
    setupModeStartTime = millis();
    WiFiProv.beginProvision(WIFI_PROV_SCHEME_BLE, WIFI_PROV_SCHEME_HANDLER_FREE_BTDM, WIFI_PROV_SECURITY_1, generatedCode.c_str(), bleName.c_str());
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

    // HTTP endpoints removed for Phase 2

    // Initial display
    Display::showStreaming(macAddress, true, true, true, 0, false, 0.0f);

    // Create FreeRTOS task for HTTP uploads
    uploadQueue = xQueueCreate(2, sizeof(SensorBatch*));
    xTaskCreatePinnedToCore(
        uploadTaskCode,
        "UploadTask",
        8192,
        NULL,
        1,
        &uploadTaskHandle,
        0
    );

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
    if (!lpFilterInit) { lpFilterState = newValue; lpFilterInit = true; }
    lpFilterState = LP_ALPHA * newValue + (1.0f - LP_ALPHA) * lpFilterState;
    return lpFilterState;
}

// Build the biquad notch coefficients once (RBJ band-stop at NOTCH_FREQ)
void initNotch() {
    float w0    = 2.0f * PI * NOTCH_FREQ / SAMPLE_RATE;
    float cw    = cosf(w0);
    float alpha = sinf(w0) / (2.0f * NOTCH_Q);
    float a0    = 1.0f + alpha;
    n_b0 = 1.0f / a0;          n_b1 = -2.0f * cw / a0;   n_b2 = 1.0f / a0;
    n_a1 = -2.0f * cw / a0;    n_a2 = (1.0f - alpha) / a0;
    n_z1 = n_z2 = 0.0f;        notchReady = true;
}

// 50 Hz notch (transposed Direct-Form II). Leaves DC and the plant band intact.
float applyNotch(float x) {
    if (!notchReady) initNotch();
    float y = n_b0 * x + n_z1;
    n_z1 = n_b1 * x - n_a1 * y + n_z2;
    n_z2 = n_b2 * x - n_a2 * y;
    return y;
}

// ── High-Frequency Data Streaming ─────────────
// 380 Hz sampling with AD8232 artifact detection

void streamReadings() {
    // Handle incoming requests (DELETE, health) non-blocking

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
        filteredMv = applyNotch(filteredMv);   // 50 Hz mains notch

        // 3. Artifact detection
        uint8_t flags = FLAG_VALID;
        bool isInvalid = false;

        // Lead-off detection (AD8232 LOD+ / LOD-)
        // NOTE: LOD pins are informational only — the AD8232 is calibrated for
        // low-impedance skin (EKG). Plant electrodes have much higher impedance
        // and trigger false lead-off permanently. We record the flag for
        // metadata but do NOT invalidate samples based on it.
        if (lp == HIGH || lm == HIGH) {
            flags |= FLAG_LEAD_OFF;
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
        buffers[currentBuffer].sampleBuffer[bufferIndex] = filteredMv;
        buffers[currentBuffer].lpBuffer[bufferIndex]     = lp;
        buffers[currentBuffer].lmBuffer[bufferIndex]     = lm;
        buffers[currentBuffer].flagBuffer[bufferIndex]   = flags;
        bufferIndex++;

        // 5. Send batch when full (380 samples = 1 second)
        if (bufferIndex >= BATCH_SIZE) {
            // Calculate batch mean for display
            float batchMean = 0.0f;
            for (int i = 0; i < BATCH_SIZE; i++) batchMean += buffers[currentBuffer].sampleBuffer[i];
            batchMean /= BATCH_SIZE;

            SensorBatch* readyBatch = &buffers[currentBuffer];
            if (uploadQueue != NULL) {
                if (xQueueSend(uploadQueue, &readyBatch, 0) != pdTRUE) {
                    Serial.println("[Biolingo] Upload queue full! Dropping batch.");
                }
            }

            currentBuffer = (currentBuffer + 1) % 2;
            bufferIndex = 0;

            // Update display after each batch (once per second)
            bool wifiOk = (WiFi.status() == WL_CONNECTED);
            Display::showStreaming(macAddress, wifiOk, true,
                                  lastSendOk, streamErrors, currentLeadOff,
                                  batchMean);

            if (streamErrors > 20) {
                Serial.println("[Biolingo] Too many errors, rebooting");
                Display::showError("Too many TX", "errors! Reboot");
                delay(2000);
                ESP.restart();
            }
        }
    }
}

void uploadTaskCode(void *pvParameters) {
    SensorBatch* batchToUpload;
    for (;;) {
        if (xQueueReceive(uploadQueue, &batchToUpload, portMAX_DELAY) == pdTRUE) {
            sendBatch(batchToUpload);
        }
    }
}

void sendBatch(SensorBatch* batch) {
    // Build JSON payload (standard production format for /api/v1/ingest)
    JsonDocument doc;
    doc["mac_address"] = macAddress;
    doc["sample_rate"] = SAMPLE_RATE;

    JsonArray readings = doc["readings"].to<JsonArray>();
    for (int i = 0; i < BATCH_SIZE; i++) {
        JsonObject r = readings.add<JsonObject>();
        r["kind"]  = "bio_signal";
        r["value"] = serialized(String(batch->sampleBuffer[i], 1));
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
    }
}
