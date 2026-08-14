/**
 * GreenMind Biolingo v22 — OLED Display Manager
 *
 * SSD1306 128x64 I2C on ESP32-S3 custom pins (SCL=IO12, SDA=IO13).
 * Uses Adafruit SSD1306 + GFX library for rendering.
 *
 * Font: built-in 6x8 at textSize(1) → 21 chars/line, 8 lines.
 */

#include "display.h"
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

static Adafruit_SSD1306 oled(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);
static bool displayReady = false;

// ── Helpers ───────────────────────────────────

/// Clear display and set cursor to top-left.
static void clearScreen() {
    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setTextColor(SSD1306_WHITE);
    oled.setCursor(0, 0);
}

/// Draw a horizontal separator line at the given y position.
static void drawSeparator(int y) {
    oled.drawFastHLine(0, y, OLED_WIDTH, SSD1306_WHITE);
}

/// Center text horizontally on a given y line (textSize 1, 6px/char).
static void centerText(const char* text, int y) {
    int len = strlen(text);
    int x = (OLED_WIDTH - len * 6) / 2;
    if (x < 0) x = 0;
    oled.setCursor(x, y);
    oled.print(text);
}

// ── Public API ────────────────────────────────

bool Display::init() {
    Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);

    if (!oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
        Serial.println("[Display] SSD1306 not found");
        displayReady = false;
        return false;
    }

    displayReady = true;
    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setTextColor(SSD1306_WHITE);
    oled.display();

    Serial.println("[Display] SSD1306 initialized");
    return true;
}

void Display::showBoot(const String& mac) {
    if (!displayReady) return;
    clearScreen();

    // Header (larger text)
    oled.setTextSize(2);
    centerText("GreenMind", 0);

    oled.setTextSize(1);
    centerText("Biolingo v22", 20);

    drawSeparator(32);

    // MAC address
    oled.setCursor(0, 36);
    oled.print("MAC:");
    oled.setCursor(0, 46);
    oled.print(mac);

    // Version
    oled.setCursor(0, 56);
    oled.printf("FW: %s  OTA", FIRMWARE_VERSION);

    oled.display();
}

void Display::showBleProvisioning(const String& name, const String& code) {
    if (!displayReady) return;
    clearScreen();

    oled.setTextSize(2);
    centerText("BLE", 0);
    centerText("Setup", 16);

    oled.setTextSize(1);
    drawSeparator(34);

    oled.setCursor(0, 38);
    oled.print(name);

    oled.setCursor(0, 50);
    oled.print("Code: ");
    oled.print(code);

    oled.display();
}

void Display::showConnecting(const String& ssid) {
    if (!displayReady) return;
    clearScreen();

    centerText("Connecting...", 10);

    oled.setCursor(0, 28);
    oled.print("SSID:");
    oled.setCursor(0, 38);
    // Truncate SSID if too long for display
    if (ssid.length() > 21) {
        oled.print(ssid.substring(0, 18));
        oled.print("...");
    } else {
        oled.print(ssid);
    }

    oled.display();
}

void Display::showSearchGW(const String& method) {
    if (!displayReady) return;
    clearScreen();

    centerText("Searching", 10);
    centerText("Gateway...", 22);

    drawSeparator(34);

    oled.setCursor(0, 40);
    oled.print("Method: ");
    oled.print(method);

    oled.display();
}

void Display::showOtaCheck() {
    if (!displayReady) return;
    clearScreen();

    centerText("OTA Check", 10);
    centerText("Please wait...", 28);

    oled.display();
}

void Display::showOtaUpdate(const String& newVersion) {
    if (!displayReady) return;
    clearScreen();

    centerText("OTA Update", 6);
    drawSeparator(18);

    oled.setCursor(0, 24);
    oled.printf("Current: %s", FIRMWARE_VERSION);
    oled.setCursor(0, 36);
    oled.print("New:     ");
    oled.print(newVersion);

    centerText("DO NOT UNPLUG!", 52);

    oled.display();
}

void Display::showStreaming(const String& mac, bool wifiOk, bool gwOk,
                            bool sendOk, int errorCount, bool leadOff,
                            float currentMv) {
    if (!displayReady) return;
    clearScreen();

    // Header
    oled.setCursor(0, 0);
    oled.print("GreenMind");
    oled.setCursor(78, 0);
    oled.printf("v%s", FIRMWARE_VERSION);

    drawSeparator(9);

    // MAC address
    oled.setCursor(0, 12);
    oled.print(mac);

    // WiFi + Gateway status
    oled.setCursor(0, 24);
    oled.print("WiFi:");
    oled.print(wifiOk ? "OK" : "--");
    oled.setCursor(64, 24);
    oled.print("GW:");
    oled.print(gwOk ? "OK" : "--");

    drawSeparator(33);

    // Streaming status + live mV value
    oled.setCursor(0, 36);
    oled.print("TX:");
    oled.print(sendOk ? "OK" : "ERR");

    // Live mV value, right-aligned (always shown, even during lead-off)
    char mvBuf[12];
    snprintf(mvBuf, sizeof(mvBuf), "%.1fmV", currentMv);
    int mvLen = strlen(mvBuf);
    int mvX = OLED_WIDTH - mvLen * 6;  // 6px per char at textSize 1
    if (mvX < 42) mvX = 42;            // Don't overlap TX status
    oled.setCursor(mvX, 36);
    oled.print(mvBuf);

    // Error count + Lead-off indicator
    oled.setCursor(0, 48);
    oled.printf("Err:%-3d", errorCount);

    oled.setCursor(64, 48);
    oled.print("LO:");
    if (leadOff) {
        oled.print("!! OFF");
    } else {
        oled.print("OK");
    }

    // Active indicator (blinking dot)
    static bool dotState = false;
    dotState = !dotState;
    if (dotState) {
        oled.fillCircle(122, 58, 3, SSD1306_WHITE);
    }

    oled.display();
}

void Display::showError(const String& line1, const String& line2) {
    if (!displayReady) return;
    clearScreen();

    oled.setTextSize(2);
    centerText("ERROR", 0);

    oled.setTextSize(1);
    drawSeparator(20);

    oled.setCursor(0, 26);
    oled.print(line1);
    oled.setCursor(0, 38);
    oled.print(line2);

    centerText("Rebooting...", 52);

    oled.display();
}

void Display::showRegistering() {
    if (!displayReady) return;
    clearScreen();

    centerText("Registering", 14);
    centerText("with Gateway...", 28);

    oled.display();
}
