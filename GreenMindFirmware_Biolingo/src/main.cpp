/**
 * GreenMind Biolingo v22 — ESP32-S3 Sensor Firmware
 *
 * Hardware: ESP32-S3-WROOM-1 + AD8232 + SSD1306 OLED (128x64 I2C)
 * Board:    Biolingo v22 (KiCad Rev v22)
 *
 * Features:
 *   - BLE setup for WiFi provisioning
 *   - 380 Hz ADC sampling with low-pass and 50 Hz notch filtering
 *   - AD8232 lead-off detection + artifact flagging
 *   - Batch POST to gateway (/api/v1/ingest, 380 samples/s)
 *   - Gateway discovery: cached IP → UDP broadcast → subnet scan
 *   - OTA firmware updates via local Raspberry Pi gateway
 *   - SSD1306 OLED status display (MAC, WiFi, GW, TX, lead-off)
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
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiProv.h>
#include <HTTPClient.h>
#include <WiFiUdp.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <esp_wifi.h>
#include <sys/time.h>
#include "ota_client.h"
#include "display.h"
#include "measurement_batch.h"
#include "sensor_spool.h"
#include "../../transport_validation/Acknowledgement.h"

// ── Pin Configuration (Biolingo v22) ──────────
static const int ADC_PIN = 4;      // IO4, ADC1_CH3
static const int LO_PLUS_PIN = 5;  // IO5, AD8232 LOD+
static const int LO_MINUS_PIN = 6; // IO6, AD8232 LOD-

// ── ADC & Sampling ───────────────────────────
static const int SAMPLE_RATE = GREENMIND_SAMPLE_RATE;
static const int BATCH_SIZE = GREENMIND_BATCH_SAMPLES;
// ESP32-S3 APB clock: 80 MHz / divider 2 / 105263 = 380.0006 Hz.
static const uint64_t SAMPLE_TIMER_TICKS = 105263;
static const float ADC_VOLTAGE_REF = 3.3f;
static const uint8_t MAX_REGISTRATION_ATTEMPTS = 5;
static const uint32_t REGISTRATION_RETRY_INTERVAL_MS = 60000;

// ── AD8232 Artifact Detection Thresholds ──────
static const float RAIL_HIGH_THRESHOLD = 3200.0f; // mV
static const float RAIL_LOW_THRESHOLD = 100.0f;   // mV
static const float JUMP_THRESHOLD = 500.0f;       // mV
static const int RECOVERY_SAMPLES_COUNT = 38;     // ~100 ms recovery window

// ── Artifact Flag Bitmask ─────────────────────
#define FLAG_VALID 0
#define FLAG_LEAD_OFF 1
#define FLAG_RAIL_HIGH 2
#define FLAG_RAIL_LOW 4
#define FLAG_JUMP 8
#define FLAG_RECOVERY 16

// ── Globals ───────────────────────────────────
Preferences prefs;
WiFiUDP udp;

String wifiSSID;
String wifiPass;
String pairingCode;
String gatewayIP;
String macAddress;

bool isProvisioned = false;

// ── Sampling Buffers & Queues ─────────────────
using SensorBatch = MeasurementBatch;

// A batch has exactly one owner at a time: the sampler, the upload queue/task,
// or the free queue. Queueing indices by value prevents the sampler from
// overwriting a buffer while the HTTP task still serializes it.
static const uint8_t BATCH_POOL_SIZE = 8;
static const uint8_t INVALID_BATCH_INDEX = UINT8_MAX;
static SensorBatch batchPool[BATCH_POOL_SIZE];
static uint8_t activeBatchIndex = INVALID_BATCH_INDEX;
static int bufferIndex = 0;
static QueueHandle_t freeBatchQueue = NULL;
static QueueHandle_t uploadQueue = NULL;
static TaskHandle_t uploadTaskHandle = NULL;
static TaskHandle_t registrationTaskHandle = NULL;
static TaskHandle_t samplingTaskHandle = NULL;
static TaskHandle_t connectivityTaskHandle = NULL;
static hw_timer_t* samplingTimer = NULL;
static SemaphoreHandle_t gatewayStateMutex = NULL;
static volatile uint32_t droppedSampleCount = 0;
static volatile uint32_t droppedBatchCount = 0;
static portMUX_TYPE droppedCounterMux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t nextBatchSequence = 0;
static uint32_t bootId = 0;

static unsigned long setupModeStartTime = 0;
static float lastValidValue = -1.0f;
static int recoveryCounter = 0;

// ── Signal Filter ─────────────────────────────
static const float LP_ALPHA = 0.25f; // Gloor ~20 Hz IIR lowpass (DC-preserving)
static float lpFilterState = 0.0f;   // first-stage 50 Hz attenuation
static bool lpFilterInit = false;

// 50 Hz mains notch (biquad, second stage, applied after the lowpass)
static const float NOTCH_FREQ = 50.0f;     // mains frequency (Hz)
static const float NOTCH_Q = 30.0f;        // notch sharpness (higher = narrower)
static float n_b0, n_b1, n_b2, n_a1, n_a2; // normalized biquad coefficients
static float n_z1 = 0.0f, n_z2 = 0.0f;     // transposed-DF2 state
static bool notchReady = false;

// ── OTA Timing ────────────────────────────────
static unsigned long lastOtaCheck = 0;
static const unsigned long OTA_CHECK_INTERVAL = 3600000; // 1 hour

// ── Streaming State (for display) ─────────────
static volatile bool lastSendOk = false;
static volatile int streamErrors = 0;
static volatile bool currentLeadOff = false;
static volatile float latestBatchMean = 0.0f;
static volatile bool displayUpdatePending = false;
static uint32_t nextGatewayUploadAttemptMs = 0;
static uint32_t gatewayUploadBackoffMs = 1000;

// ── Forward Declarations ──────────────────────
void startSetupMode();
void startRuntimeMode();
String getMacAddress();
void saveConfig();
bool discoverGateway();
bool checkGatewayHealth(const String& ip);
int registerSensor();
void streamReadings();
bool acquireFreeBatch();
void recordDroppedSamples(uint32_t count, const char* reason);
bool sendBatch(const SensorBatch& batch);
void uploadTaskCode(void* pvParameters);
void registrationTaskCode(void* pvParameters);
void samplingTaskCode(void* pvParameters);
void connectivityTaskCode(void* pvParameters);
void startSamplingTimer();
void IRAM_ATTR onSamplingTimer();
String currentGatewayAddress();
void updateGatewayAddress(const String& address);

// ── Helpers für BLE Provisioning ──────────────
String generatePairingCode() {
    static const char alphabet[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    String code = "";
    for (int i = 0; i < 6; i++) {
        code += alphabet[esp_random() % (sizeof(alphabet) - 1)];
    }
    return code;
}

// ══════════════════════════════════════════════
//  SETUP & LOOP
// ══════════════════════════════════════════════

void setup() {
    Serial.begin(115200);
    delay(500);
    bootId = esp_random();
    Serial.printf("\n[Biolingo] Booting v%s (OTA enabled)\n", FIRMWARE_VERSION);
    Serial.printf("[Biolingo] Flash: %u MB, PSRAM: %u MB\n",
                  static_cast<unsigned>(ESP.getFlashChipSize() / (1024 * 1024)),
                  static_cast<unsigned>(ESP.getPsramSize() / (1024 * 1024)));
    gatewayStateMutex = xSemaphoreCreateMutex();

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
    wifiSSID = prefs.getString("ssid", "");
    wifiPass = prefs.getString("pass", "");
    pairingCode = prefs.getString("code", "");
    gatewayIP = prefs.getString("gwip", "");
    prefs.end();

    // Open Wi-Fi networks legitimately have an empty passphrase. A persisted
    // SSID is the provisioning marker; WiFi.begin handles open networks.
    isProvisioned = (wifiSSID.length() > 0);

    Serial.printf("[Biolingo] MAC: %s  Provisioned: %s\n", macAddress.c_str(),
                  isProvisioned ? "yes" : "no");
    Serial.printf("[Biolingo] Cached GW: '%s'  Pairing: %s\n", gatewayIP.c_str(),
                  pairingCode.length() > 0 ? "pending" : "none");

    delay(1500); // Let user see boot screen

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
        String gatewayAddress = currentGatewayAddress();
        if (displayUpdatePending) {
            displayUpdatePending = false;
            Display::showStreaming(macAddress, WiFi.status() == WL_CONNECTED,
                                   gatewayAddress.length() > 0, lastSendOk, streamErrors,
                                   currentLeadOff, latestBatchMean);
        }

        // Periodic OTA check
        if (WiFi.status() == WL_CONNECTED && gatewayAddress.length() > 0 &&
            millis() - lastOtaCheck > OTA_CHECK_INTERVAL) {
            lastOtaCheck = millis();
            Serial.println("[Biolingo] Periodic OTA check...");
            Display::showOtaCheck();
            GreenMindOTA::checkAndUpdate(gatewayAddress);
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
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3],
             mac[4], mac[5]);
    return String(buf);
}

String currentGatewayAddress() {
    if (gatewayStateMutex == NULL)
        return gatewayIP;
    xSemaphoreTake(gatewayStateMutex, portMAX_DELAY);
    String address = gatewayIP;
    xSemaphoreGive(gatewayStateMutex);
    return address;
}

void updateGatewayAddress(const String& address) {
    if (gatewayStateMutex == NULL) {
        gatewayIP = address;
        return;
    }
    xSemaphoreTake(gatewayStateMutex, portMAX_DELAY);
    gatewayIP = address;
    xSemaphoreGive(gatewayStateMutex);
}

void saveConfig() {
    prefs.begin("gm", false);
    prefs.putString("ssid", wifiSSID);
    prefs.putString("pass", wifiPass);
    prefs.putString("code", pairingCode);
    prefs.putString("gwip", currentGatewayAddress());
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
    Serial.printf("[Biolingo] BLE provisioning name: %s\n", bleName.c_str());

    Display::showBleProvisioning(bleName, generatedCode);

    setupModeStartTime = millis();
    WiFiProv.beginProvision(WIFI_PROV_SCHEME_BLE, WIFI_PROV_SCHEME_HANDLER_FREE_BTDM,
                            WIFI_PROV_SECURITY_1, generatedCode.c_str(), bleName.c_str());
}

// ══════════════════════════════════════════════
//  RUNTIME MODE
// ══════════════════════════════════════════════

void startRuntimeMode() {
    Serial.printf("[Biolingo] Connecting to WiFi: %s\n", wifiSSID.c_str());
    Display::showConnecting(wifiSSID);

    WiFi.mode(WIFI_STA);
    WiFi.begin(wifiSSID.c_str(), wifiPass.c_str());
    WiFi.setSleep(false);
    esp_wifi_set_ps(WIFI_PS_NONE);

    int retries = 10;
    while (WiFi.status() != WL_CONNECTED && retries-- > 0) {
        delay(1000);
        Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[Biolingo] WiFi unavailable; starting durable offline capture");
        Display::showError("WiFi offline", "Buffering data");
    } else {
        Serial.printf("[Biolingo] WiFi OK  IP: %s\n", WiFi.localIP().toString().c_str());
    }

    String gatewayAddress = currentGatewayAddress();
    bool gatewayReady = WiFi.status() == WL_CONNECTED && gatewayAddress.length() > 0 &&
                        checkGatewayHealth(gatewayAddress);
    if (gatewayReady) {
        Serial.printf("[Biolingo] Gateway at %s\n", gatewayAddress.c_str());
        Serial.println("[Biolingo] Boot OTA check...");
        Display::showOtaCheck();
        GreenMindOTA::checkAndUpdate(gatewayAddress);
        lastOtaCheck = millis();
    } else {
        updateGatewayAddress("");
        Serial.println("[Biolingo] Gateway unavailable; discovery continues in background");
    }

    if (!sensorSpool.begin()) {
        Serial.println("[Biolingo] Persistent spool unavailable; live upload only");
    }

    // Initial display
    Display::showStreaming(macAddress, WiFi.status() == WL_CONNECTED, gatewayReady, gatewayReady, 0,
                           false, 0.0f);

    // Create a fixed batch pool and transfer ownership with queue indices.
    freeBatchQueue = xQueueCreate(BATCH_POOL_SIZE, sizeof(uint8_t));
    uploadQueue = xQueueCreate(BATCH_POOL_SIZE, sizeof(uint8_t));
    if (freeBatchQueue == NULL || uploadQueue == NULL) {
        Serial.println("[Biolingo] Failed to allocate batch queues. Rebooting...");
        Display::showError("Queue alloc", "failed! Reboot");
        delay(2000);
        ESP.restart();
    }

    for (uint8_t i = 0; i < BATCH_POOL_SIZE; ++i) {
        if (xQueueSend(freeBatchQueue, &i, 0) != pdTRUE) {
            Serial.println("[Biolingo] Failed to initialize batch pool. Rebooting...");
            delay(2000);
            ESP.restart();
        }
    }

    if (!acquireFreeBatch()) {
        Serial.println("[Biolingo] No initial batch buffer available. Rebooting...");
        delay(2000);
        ESP.restart();
    }

    BaseType_t taskResult =
        xTaskCreatePinnedToCore(uploadTaskCode, "UploadTask", 8192, NULL, 1, &uploadTaskHandle, 0);
    if (taskResult != pdPASS) {
        Serial.println("[Biolingo] Failed to start upload task. Rebooting...");
        Display::showError("Upload task", "failed! Reboot");
        delay(2000);
        ESP.restart();
    }

    BaseType_t connectivityResult = xTaskCreatePinnedToCore(
        connectivityTaskCode, "ConnectivityTask", 6144, NULL, 1, &connectivityTaskHandle, 0);
    if (connectivityResult != pdPASS) {
        Serial.println("[Biolingo] Connectivity task unavailable; cached gateway only");
    }

    if (pairingCode.length() > 0 && gatewayReady) {
        BaseType_t registrationTaskResult = xTaskCreatePinnedToCore(
            registrationTaskCode, "RegistrationTask", 6144, NULL, 1, &registrationTaskHandle, 0);
        if (registrationTaskResult != pdPASS) {
            Serial.println("[Biolingo] Registration task unavailable; pairing retained");
        }
    }

    BaseType_t samplingResult = xTaskCreatePinnedToCore(samplingTaskCode, "SamplingTask", 6144,
                                                        NULL, 3, &samplingTaskHandle, 1);
    if (samplingResult != pdPASS) {
        Serial.println("[Biolingo] Failed to start sampling task. Rebooting...");
        delay(2000);
        ESP.restart();
    }
    startSamplingTimer();
    Serial.printf("[Biolingo] Hardware-timed streaming started (v%s, %d Hz)\n", FIRMWARE_VERSION,
                  SAMPLE_RATE);
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
    if (code != 200 || body.indexOf("hardware_id") < 0)
        return false;

    JsonDocument status;
    if (deserializeJson(status, body) == DeserializationError::Ok) {
        uint64_t gatewayEpochMs = status["utc_epoch_ms"] | 0ULL;
        if (gatewayEpochMs >= 1577836800000ULL) {
            timeval now{};
            now.tv_sec = static_cast<time_t>(gatewayEpochMs / 1000ULL);
            now.tv_usec = static_cast<suseconds_t>((gatewayEpochMs % 1000ULL) * 1000ULL);
            settimeofday(&now, nullptr);
        }
    }
    return true;
}

bool discoverGateway() {
    String cachedGateway = currentGatewayAddress();
    // 1) Cached IP
    if (cachedGateway.length() > 0) {
        Serial.printf("[Biolingo] Trying cached: %s\n", cachedGateway.c_str());
        if (checkGatewayHealth(cachedGateway))
            return true;
        updateGatewayAddress("");
    }

    // 2) UDP broadcast
    Serial.println("[Biolingo] UDP discovery...");

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
                    Serial.printf("[Biolingo] UDP reply: payload=%s source=%s\n", payloadIP.c_str(),
                                  sourceIP.c_str());
                    disc.stop();
                    if (checkGatewayHealth(sourceIP)) {
                        updateGatewayAddress(sourceIP);
                    } else if (checkGatewayHealth(payloadIP)) {
                        updateGatewayAddress(payloadIP);
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

    IPAddress myIP = WiFi.localIP();
    for (int host = 1; host < 255; host++) {
        IPAddress candidate(myIP[0], myIP[1], myIP[2], host);
        if (candidate == myIP)
            continue;
        yield();

        WiFiClient client;
        client.setTimeout(100);
        if (client.connect(candidate, 80)) {
            client.print("GET /api/v1/health HTTP/1.0\r\nHost: gm\r\n\r\n");
            unsigned long t = millis();
            while (!client.available() && millis() - t < 300) {
                delay(10);
                yield();
            }
            String resp = client.readString();
            client.stop();
            if (resp.indexOf("hardware_id") >= 0) {
                updateGatewayAddress(candidate.toString());
                Serial.printf("[Biolingo] Found via scan: %s\n", currentGatewayAddress().c_str());
                saveConfig();
                return true;
            }
        }
        if (host % 20 == 0)
            Serial.printf("[Biolingo] Scanned %d/254\n", host);
    }
    return false;
}

// ── Sensor Registration ───────────────────────

int registerSensor() {
    Serial.println("[Biolingo] Registering sensor with gateway");

    JsonDocument doc;
    doc["mac_address"] = macAddress;
    doc["code"] = pairingCode;

    String body;
    serializeJson(doc, body);

    HTTPClient http;
    String url = "http://" + currentGatewayAddress() + "/api/v1/sensors/register";
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(10000);

    int code = http.POST(body);
    http.end();
    return code;
}

void registrationTaskCode(void* pvParameters) {
    for (uint8_t attempt = 1; attempt <= MAX_REGISTRATION_ATTEMPTS; ++attempt) {
        int code = registerSensor();
        if (code == 200 || code == 201) {
            Serial.println("[Biolingo] Sensor registered");
            pairingCode = "";
            saveConfig();
            break;
        }

        bool permanentFailure = (code == 400 || code == 403 || code == 409 || code == 422);
        if (permanentFailure) {
            Serial.printf("[Biolingo] Registration permanently rejected: HTTP %d\n", code);
            pairingCode = "";
            saveConfig();
            break;
        }

        Serial.printf("[Biolingo] Registration attempt %u/%u failed: HTTP %d\n", attempt,
                      MAX_REGISTRATION_ATTEMPTS, code);
        if (attempt < MAX_REGISTRATION_ATTEMPTS) {
            vTaskDelay(pdMS_TO_TICKS(REGISTRATION_RETRY_INTERVAL_MS));
        }
    }

    registrationTaskHandle = NULL;
    vTaskDelete(NULL);
}

// ── Signal Filtering ──────────────────────────
// 3-sample moving average for noise reduction

float applyFilter(float newValue) {
    if (!lpFilterInit) {
        lpFilterState = newValue;
        lpFilterInit = true;
    }
    lpFilterState = LP_ALPHA * newValue + (1.0f - LP_ALPHA) * lpFilterState;
    return lpFilterState;
}

// Build the biquad notch coefficients once (RBJ band-stop at NOTCH_FREQ)
void initNotch() {
    float w0 = 2.0f * PI * NOTCH_FREQ / SAMPLE_RATE;
    float cw = cosf(w0);
    float alpha = sinf(w0) / (2.0f * NOTCH_Q);
    float a0 = 1.0f + alpha;
    n_b0 = 1.0f / a0;
    n_b1 = -2.0f * cw / a0;
    n_b2 = 1.0f / a0;
    n_a1 = -2.0f * cw / a0;
    n_a2 = (1.0f - alpha) / a0;
    n_z1 = n_z2 = 0.0f;
    notchReady = true;
}

// 50 Hz notch (transposed Direct-Form II). Leaves DC and the plant band intact.
float applyNotch(float x) {
    if (!notchReady)
        initNotch();
    float y = n_b0 * x + n_z1;
    n_z1 = n_b1 * x - n_a1 * y + n_z2;
    n_z2 = n_b2 * x - n_a2 * y;
    return y;
}

bool acquireFreeBatch() {
    if (freeBatchQueue == NULL || activeBatchIndex != INVALID_BATCH_INDEX) {
        return activeBatchIndex != INVALID_BATCH_INDEX;
    }

    uint8_t nextBatch = INVALID_BATCH_INDEX;
    if (xQueueReceive(freeBatchQueue, &nextBatch, 0) != pdTRUE) {
        return false;
    }
    if (nextBatch >= BATCH_POOL_SIZE) {
        Serial.println("[Biolingo] Invalid batch-pool index. Rebooting...");
        delay(1000);
        ESP.restart();
        return false;
    }

    activeBatchIndex = nextBatch;
    bufferIndex = 0;
    batchPool[nextBatch] = SensorBatch{};
    batchPool[nextBatch].bootId = bootId;
    batchPool[nextBatch].sampleRate = GREENMIND_SAMPLE_RATE;
    batchPool[nextBatch].sampleCount = GREENMIND_BATCH_SAMPLES;
    batchPool[nextBatch].protocolVersion = GREENMIND_PROTOCOL_VERSION;
    strlcpy(batchPool[nextBatch].firmwareVersion, FIRMWARE_VERSION,
            sizeof(batchPool[nextBatch].firmwareVersion));
    strlcpy(batchPool[nextBatch].calibrationVersion, "nominal-adc-3v3-v1",
            sizeof(batchPool[nextBatch].calibrationVersion));
    return true;
}

void recordDroppedSamples(uint32_t count, const char* reason) {
    portENTER_CRITICAL(&droppedCounterMux);
    uint32_t previousBatchEquivalent = droppedSampleCount / BATCH_SIZE;
    droppedSampleCount += count;
    uint32_t newBatchEquivalent = droppedSampleCount / BATCH_SIZE;
    droppedBatchCount = newBatchEquivalent;
    uint32_t totalSamples = droppedSampleCount;
    uint32_t totalBatches = droppedBatchCount;
    portEXIT_CRITICAL(&droppedCounterMux);

    if (newBatchEquivalent > previousBatchEquivalent) {
        Serial.printf("[Biolingo] DATA LOSS: %lu samples (~%lu batches) dropped; %s\n",
                      static_cast<unsigned long>(totalSamples),
                      static_cast<unsigned long>(totalBatches), reason);
    }
}

// ── High-Frequency Data Streaming ─────────────
// 380 Hz sampling with AD8232 artifact detection

void streamReadings() {
    int rawAdc = analogRead(ADC_PIN);
    uint8_t lp = digitalRead(LO_PLUS_PIN);
    uint8_t lm = digitalRead(LO_MINUS_PIN);

    float mv = (rawAdc / 4095.0f) * ADC_VOLTAGE_REF * 1000.0f;
    float filteredMv = applyNotch(applyFilter(mv));
    uint8_t flags = FLAG_VALID;
    bool isInvalid = false;

    if (lp == HIGH || lm == HIGH) {
        flags |= FLAG_LEAD_OFF;
        currentLeadOff = true;
    } else {
        currentLeadOff = false;
    }
    if (filteredMv > RAIL_HIGH_THRESHOLD) {
        flags |= FLAG_RAIL_HIGH;
        isInvalid = true;
    }
    if (filteredMv < RAIL_LOW_THRESHOLD) {
        flags |= FLAG_RAIL_LOW;
        isInvalid = true;
    }
    if (!isInvalid && lastValidValue >= 0 && fabs(filteredMv - lastValidValue) > JUMP_THRESHOLD) {
        flags |= FLAG_JUMP;
        isInvalid = true;
    }
    if (isInvalid) {
        recoveryCounter = RECOVERY_SAMPLES_COUNT;
    } else if (recoveryCounter > 0) {
        flags |= FLAG_RECOVERY;
        recoveryCounter--;
        isInvalid = true;
    }
    if (!isInvalid)
        lastValidValue = filteredMv;

    if (!acquireFreeBatch()) {
        recordDroppedSamples(1, "all batch buffers are in flight");
        return;
    }

    SensorBatch& activeBatch = batchPool[activeBatchIndex];
    float boundedMv = max(0.0f, min(filteredMv, 6553.5f));
    activeBatch.samplesDeciMv[bufferIndex] = static_cast<uint16_t>(lroundf(boundedMv * 10.0f));
    const uint8_t invalidMask =
        FLAG_LEAD_OFF | FLAG_RAIL_HIGH | FLAG_RAIL_LOW | FLAG_JUMP | FLAG_RECOVERY;
    if ((flags & invalidMask) == 0)
        activeBatch.quality.valid++;
    if (flags & FLAG_LEAD_OFF)
        activeBatch.quality.leadOff++;
    if (flags & FLAG_RAIL_HIGH)
        activeBatch.quality.railHigh++;
    if (flags & FLAG_RAIL_LOW)
        activeBatch.quality.railLow++;
    if (flags & FLAG_JUMP)
        activeBatch.quality.jump++;
    if (flags & FLAG_RECOVERY)
        activeBatch.quality.recovery++;
    bufferIndex++;

    if (bufferIndex < BATCH_SIZE)
        return;

    activeBatch.sequence = nextBatchSequence++;
    activeBatch.uptimeMs = millis();
    portENTER_CRITICAL(&droppedCounterMux);
    activeBatch.droppedSamplesTotal = droppedSampleCount;
    portEXIT_CRITICAL(&droppedCounterMux);

    timeval captured{};
    gettimeofday(&captured, nullptr);
    if (captured.tv_sec >= 1577836800) {
        activeBatch.capturedAtEpochMs = static_cast<uint64_t>(captured.tv_sec) * 1000ULL +
                                        static_cast<uint64_t>(captured.tv_usec / 1000);
    }

    uint64_t sumDeciMv = 0;
    for (int index = 0; index < BATCH_SIZE; ++index)
        sumDeciMv += activeBatch.samplesDeciMv[index];
    latestBatchMean = static_cast<float>(sumDeciMv) / (BATCH_SIZE * 10.0f);
    displayUpdatePending = true;

    uint8_t readyBatchIndex = activeBatchIndex;
    activeBatchIndex = INVALID_BATCH_INDEX;
    bufferIndex = 0;
    if (uploadQueue == NULL || xQueueSend(uploadQueue, &readyBatchIndex, 0) != pdTRUE) {
        recordDroppedSamples(BATCH_SIZE, "upload queue rejected a completed batch");
        if (freeBatchQueue != NULL)
            xQueueSend(freeBatchQueue, &readyBatchIndex, 0);
    }
    acquireFreeBatch();
}

void uploadTaskCode(void* pvParameters) {
    uint8_t batchToUpload = INVALID_BATCH_INDEX;
    for (;;) {
        if (xQueueReceive(uploadQueue, &batchToUpload, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (batchToUpload >= BATCH_POOL_SIZE) {
                Serial.println("[Biolingo] Upload queue returned an invalid batch index.");
                continue;
            }

            SensorBatch& liveBatch = batchPool[batchToUpload];
            bool retained = false;
            if (sensorSpool.hasPending()) {
                retained = sensorSpool.append(liveBatch);
                if (!retained) {
                    SensorBatch oldest{};
                    if (sensorSpool.peek(oldest) && sendBatch(oldest) &&
                        sensorSpool.acknowledge()) {
                        retained = sensorSpool.append(liveBatch);
                    }
                }
            } else if (sendBatch(liveBatch)) {
                retained = true;
            } else {
                retained = sensorSpool.append(liveBatch);
            }

            if (!retained) {
                recordDroppedSamples(BATCH_SIZE, "persistent spool could not retain batch");
            }

            if (xQueueSend(freeBatchQueue, &batchToUpload, portMAX_DELAY) != pdTRUE) {
                Serial.println("[Biolingo] Failed to return batch buffer to pool.");
            }
        }

        for (uint8_t drained = 0; drained < 4 && sensorSpool.hasPending(); ++drained) {
            SensorBatch pending{};
            if (!sensorSpool.peek(pending) || !sendBatch(pending))
                break;
            if (!sensorSpool.acknowledge()) {
                Serial.println("[Spool] ACK retained because segment cleanup failed");
                break;
            }
        }
    }
}

bool sendBatch(const SensorBatch& batch) {
    String gatewayAddress = currentGatewayAddress();
    if (WiFi.status() != WL_CONNECTED || gatewayAddress.isEmpty())
        return false;
    if (greenmind::retryPending(millis(), nextGatewayUploadAttemptMs))
        return false;

    static JsonDocument doc;
    doc.clear();
    doc["mac_address"] = macAddress;
    doc["sample_rate"] = batch.sampleRate;
    doc["protocol_version"] = batch.protocolVersion;
    doc["firmware_version"] = batch.firmwareVersion;
    doc["calibration_version"] = batch.calibrationVersion;
    doc["boot_id"] = batch.bootId;
    doc["sequence"] = batch.sequence;
    doc["uptime_ms"] = batch.uptimeMs;
    doc["dropped_samples_total"] = batch.droppedSamplesTotal;
    if (batch.capturedAtEpochMs > 0)
        doc["captured_at_epoch_ms"] = batch.capturedAtEpochMs;

    JsonObject quality = doc["quality_counts"].to<JsonObject>();
    quality["valid"] = batch.quality.valid;
    quality["lead_off"] = batch.quality.leadOff;
    quality["rail_high"] = batch.quality.railHigh;
    quality["rail_low"] = batch.quality.railLow;
    quality["jump"] = batch.quality.jump;
    quality["recovery"] = batch.quality.recovery;

    doc["kind"] = "bio_signal";
    doc["unit"] = "mV";
    doc["value_scale_mv"] = 0.1f;
    JsonArray values = doc["values_deci_mv"].to<JsonArray>();
    for (uint16_t index = 0; index < batch.sampleCount; ++index)
        values.add(batch.samplesDeciMv[index]);

    String payload;
    serializeJson(doc, payload);

    HTTPClient http;
    String url = "http://" + gatewayAddress + "/api/v1/ingest";
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(2000);

    int code = http.POST(payload);
    String responseBody = (code == 200 || code == 201) ? http.getString() : "";
    http.end();

    if (code == 200 || code == 201) {
        JsonDocument acknowledgment;
        if (deserializeJson(acknowledgment, responseBody) != DeserializationError::Ok ||
            !greenmind::gatewayAcknowledged(acknowledgment.as<JsonVariantConst>(), batch.bootId,
                                            batch.sequence, batch.sampleCount)) {
            streamErrors++;
            lastSendOk = false;
            nextGatewayUploadAttemptMs = millis() + gatewayUploadBackoffMs;
            gatewayUploadBackoffMs = min<uint32_t>(gatewayUploadBackoffMs * 2, 60000);
            Serial.println("[Biolingo] Gateway returned an invalid batch ACK");
            return false;
        }
        streamErrors = 0;
        lastSendOk = true;
        nextGatewayUploadAttemptMs = 0;
        gatewayUploadBackoffMs = 1000;
        return true;
    } else {
        streamErrors++;
        lastSendOk = false;
        nextGatewayUploadAttemptMs = millis() + gatewayUploadBackoffMs;
        gatewayUploadBackoffMs = min<uint32_t>(gatewayUploadBackoffMs * 2, 60000);
        Serial.printf("[Biolingo] Stream error: HTTP %d (count: %d)\n", code, streamErrors);
        return false;
    }
}

void IRAM_ATTR onSamplingTimer() {
    BaseType_t higherPriorityTaskWoken = pdFALSE;
    vTaskNotifyGiveFromISR(samplingTaskHandle, &higherPriorityTaskWoken);
    if (higherPriorityTaskWoken == pdTRUE)
        portYIELD_FROM_ISR();
}

void startSamplingTimer() {
    samplingTimer = timerBegin(0, 2, true);
    if (samplingTimer == NULL) {
        Serial.println("[Biolingo] Hardware timer allocation failed. Rebooting...");
        delay(1000);
        ESP.restart();
    }
    timerAttachInterrupt(samplingTimer, &onSamplingTimer, true);
    timerAlarmWrite(samplingTimer, SAMPLE_TIMER_TICKS, true);
    timerAlarmEnable(samplingTimer);
}

void samplingTaskCode(void* pvParameters) {
    for (;;) {
        uint32_t notifications = ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (notifications > 1) {
            uint32_t partialBatchSamples = static_cast<uint32_t>(bufferIndex);
            bufferIndex = 0;
            recordDroppedSamples((notifications - 1) + partialBatchSamples,
                                 "hardware sampling task was delayed");
        }
        streamReadings();
    }
}

void connectivityTaskCode(void* pvParameters) {
    for (;;) {
        if (WiFi.status() != WL_CONNECTED) {
            WiFi.reconnect();
            WiFi.setSleep(false);
            esp_wifi_set_ps(WIFI_PS_NONE);
            for (uint8_t attempt = 0; attempt < 10 && WiFi.status() != WL_CONNECTED; ++attempt)
                vTaskDelay(pdMS_TO_TICKS(1000));
        }

        if (WiFi.status() == WL_CONNECTED) {
            String gatewayAddress = currentGatewayAddress();
            bool gatewayHealthy = gatewayAddress.length() > 0 && checkGatewayHealth(gatewayAddress);
            if (!gatewayHealthy && discoverGateway()) {
                Serial.printf("[Biolingo] Gateway rediscovered at %s\n",
                              currentGatewayAddress().c_str());
                if (pairingCode.length() > 0 && registrationTaskHandle == NULL) {
                    xTaskCreatePinnedToCore(registrationTaskCode, "RegistrationTask", 6144, NULL, 1,
                                            &registrationTaskHandle, 0);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(15000));
    }
}
