/**
 * GreenMind Biolingo v22 — OLED Display Manager
 *
 * SSD1306 128x64 I2C display on ESP32-S3 (SCL=IO12, SDA=IO13).
 * Provides context-specific screens for boot, setup, runtime, and errors.
 */

#ifndef GREENMIND_DISPLAY_H
#define GREENMIND_DISPLAY_H

#include <Arduino.h>

// ── Pin Configuration ─────────────────────────
#define OLED_SDA_PIN 13
#define OLED_SCL_PIN 12
#define OLED_WIDTH   128
#define OLED_HEIGHT  64
#define OLED_ADDR    0x3C

namespace Display {

/// Initialize I2C and SSD1306 display. Returns false if display not found.
bool init();

/// Boot screen: firmware version + MAC address.
void showBoot(const String& mac);

/// Setup mode: AP name + portal IP.
void showSetup(const String& apName);

/// WiFi connecting screen.
void showConnecting(const String& ssid);

/// Gateway discovery in progress.
void showSearchGW(const String& method);

/// OTA check / update in progress.
void showOtaCheck();
void showOtaUpdate(const String& newVersion);

/// Main streaming status screen (called once per batch send).
void showStreaming(const String& mac, bool wifiOk, bool gwOk,
                   bool sendOk, int errorCount, bool leadOff,
                   float currentMv);

/// Error / reboot screen.
void showError(const String& line1, const String& line2);

/// Registration in progress.
void showRegistering();

}  // namespace Display

#endif
