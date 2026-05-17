/*
 * GreenMind AD8232 Bio-Signal Firmware
 *
 * Samples AD8232 Output, LO+, and LO- synchronously at 380 Hz.
 * Detects Lead-off, Rail Hits, and Jump artifacts locally.
 * Sends data as compact JSON array batches to the gateway.
 */

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <HTTPClient.h>
#include <WiFiUdp.h>
#include <Preferences.h>
#include <ArduinoJson.h>

// ── Pin Configuration ─────────────────────────
static const int ADC_PIN = 34;
static const int LO_PLUS_PIN = 32;
static const int LO_MINUS_PIN = 33;

// ── AD8232 & Artifact Config ─────────────────
static const int SAMPLE_RATE = 380;
static const int SAMPLE_INTERVAL_US = 1000000 / SAMPLE_RATE;
static const int BATCH_SIZE = 380; // 1 second
static const float ADC_VOLTAGE_REF = 3.3f;

// Artifact Thresholds (in mV)
static const float RAIL_HIGH_THRESHOLD = 3200.0f;
static const float RAIL_LOW_THRESHOLD = 100.0f;
static const float JUMP_THRESHOLD = 500.0f;
static const int RECOVERY_SAMPLES_COUNT = 38; // 100ms recovery window

// Flag Bitmask Constants
#define FLAG_VALID        0
#define FLAG_LEAD_OFF     1
#define FLAG_RAIL_HIGH    2
#define FLAG_RAIL_LOW     4
#define FLAG_JUMP         8
#define FLAG_RECOVERY    16

// ── Globals ─────────────────────────────────
Preferences prefs;
WebServer   server(80);
DNSServer   dnsServer;

String wifiSSID;
String wifiPass;
String pairingCode;
String gatewayIP;
String macAddress;

bool isProvisioned = false;

// Ring Buffers & State
float sampleBuffer[BATCH_SIZE];
uint8_t lpBuffer[BATCH_SIZE];
uint8_t lmBuffer[BATCH_SIZE];
uint8_t flagBuffer[BATCH_SIZE];

int bufferIndex = 0;
unsigned long lastSampleTime = 0;
unsigned long lastWifiCheck = 0;
float lastValidValue = -1.0f;
int recoveryCounter = 0;

// Filter
static const int FILTER_SIZE = 3;
static float filterBuffer[FILTER_SIZE];
static int filterIndex = 0;

// ── Forward Declarations ────────────────────
float applyFilter(float newValue);
void sendBatch();
void factoryReset();

// ... existing Wifi/Captive Portal boilerplate omitted for brevity ...
// We include the core logic setup + loop here.

void startRuntimeMode() {
  Serial.printf("[GreenMind] Connecting to WiFi: %s\n", wifiSSID.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSSID.c_str(), wifiPass.c_str());
  int retries = 30;
  while (WiFi.status() != WL_CONNECTED && retries-- > 0) { delay(1000); }
  if (WiFi.status() != WL_CONNECTED) ESP.restart();

  Serial.println("[GreenMind] Stream started");
  lastSampleTime = micros();
}

void setup() {
  Serial.begin(115200);
  pinMode(ADC_PIN, INPUT);
  pinMode(LO_PLUS_PIN, INPUT);
  pinMode(LO_MINUS_PIN, INPUT);

  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);

  WiFi.mode(WIFI_STA);
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  macAddress = String(buf);

  prefs.begin("gm", false);
  wifiSSID = prefs.getString("ssid", "");
  wifiPass = prefs.getString("pass", "");
  gatewayIP = prefs.getString("gwip", "192.168.1.100"); // default for testing
  prefs.end();

  isProvisioned = (wifiSSID.length() > 0 && wifiPass.length() > 0);
  if (isProvisioned) startRuntimeMode();
}

void loop() {
  if (!isProvisioned) return; // captive portal logic

  if (millis() - lastWifiCheck > 5000) {
    lastWifiCheck = millis();
    if (WiFi.status() != WL_CONNECTED) {
      WiFi.reconnect();
    }
  }

  unsigned long now = micros();
  if (now - lastSampleTime >= SAMPLE_INTERVAL_US) {
    lastSampleTime += SAMPLE_INTERVAL_US; // exact timing compensation

    // 1. Read Raw
    int raw_adc = analogRead(ADC_PIN);
    uint8_t lp = digitalRead(LO_PLUS_PIN);
    uint8_t lm = digitalRead(LO_MINUS_PIN);

    // 2. Convert & Filter
    float mv = (raw_adc / 4095.0f) * ADC_VOLTAGE_REF * 1000.0f;
    float filtered_mv = applyFilter(mv);

    // 3. Artifact Detection
    uint8_t flags = FLAG_VALID;
    bool isInvalid = false;

    if (lp == HIGH || lm == HIGH) { flags |= FLAG_LEAD_OFF; isInvalid = true; }
    if (filtered_mv > RAIL_HIGH_THRESHOLD) { flags |= FLAG_RAIL_HIGH; isInvalid = true; }
    if (filtered_mv < RAIL_LOW_THRESHOLD) { flags |= FLAG_RAIL_LOW; isInvalid = true; }

    if (!isInvalid && lastValidValue >= 0) {
      if (abs(filtered_mv - lastValidValue) > JUMP_THRESHOLD) {
        flags |= FLAG_JUMP;
        isInvalid = true;
      }
    }

    // 4. Recovery State Machine
    if (isInvalid) {
      recoveryCounter = RECOVERY_SAMPLES_COUNT; // Reset recovery window
    } else if (recoveryCounter > 0) {
      flags |= FLAG_RECOVERY;
      recoveryCounter--;
      isInvalid = true; // Still considered invalid during recovery
    }

    if (!isInvalid) {
      lastValidValue = filtered_mv; // Update baseline only if perfectly valid
    }

    // 5. Store in Buffer
    sampleBuffer[bufferIndex] = filtered_mv;
    lpBuffer[bufferIndex] = lp;
    lmBuffer[bufferIndex] = lm;
    flagBuffer[bufferIndex] = flags;
    bufferIndex++;

    if (bufferIndex >= BATCH_SIZE) {
      sendBatch();
      bufferIndex = 0;
    }
  }
}

float applyFilter(float newValue) {
  filterBuffer[filterIndex] = newValue;
  filterIndex = (filterIndex + 1) % FILTER_SIZE;
  float sum = 0;
  for (int i = 0; i < FILTER_SIZE; i++) sum += filterBuffer[i];
  return sum / FILTER_SIZE;
}

void sendBatch() {
  JsonDocument doc;
  doc["mac_address"] = macAddress;
  doc["sample_rate"] = SAMPLE_RATE;
  doc["hardware"] = "AD8232";
  
  JsonArray cols = doc["columns"].to<JsonArray>();
  cols.add("out_mv");
  cols.add("lo_plus");
  cols.add("lo_minus");
  cols.add("flags");

  JsonArray readings = doc["readings"].to<JsonArray>();
  for (int i = 0; i < BATCH_SIZE; i++) {
    JsonArray row = readings.add<JsonArray>();
    row.add(serialized(String(sampleBuffer[i], 1)));
    row.add(lpBuffer[i]);
    row.add(lmBuffer[i]);
    row.add(flagBuffer[i]);
  }

  String payload;
  serializeJson(doc, payload);

  HTTPClient http;
  http.begin("http://" + gatewayIP + "/api/v1/biosignal/ingest");
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(payload);
  http.end();
  Serial.printf("Sent %d samples, HTTP %d\n", BATCH_SIZE, code);
}
