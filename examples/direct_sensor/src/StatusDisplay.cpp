#include "StatusDisplay.h"

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Wire.h>

namespace {
// Same SSD1306 wiring as the existing Biolingo-v22 firmware.
Adafruit_SSD1306 screen(128, 64, &Wire, -1);
bool displayReady = false;

void line(uint8_t y, const String& text) {
    screen.setCursor(0, y);
    screen.print(text.substring(0, 21));
}
}

bool StatusDisplay::init() {
    Wire.begin(13, 12);
    Wire.setTimeOut(25);
    Wire.beginTransmission(0x3C);
    const uint8_t probe = Wire.endTransmission();
    if (probe != 0) {
        Serial.printf("display_not_found address=0x3c i2c_error=%u\n", probe);
        return false;
    }
    // Do not reinitialise Wire: retain the board's custom SDA/SCL pins.
    displayReady = screen.begin(SSD1306_SWITCHCAPVCC, 0x3C, false, false);
    if (!displayReady) {
        Serial.println("display_buffer_allocation_failed");
        return false;
    }
    screen.ssd1306_command(SSD1306_DISPLAYON);
    show("START", "Biolingo v22", "Direct-Testfirmware", "Sensor startet...", "");
    Serial.println("display_ready address=0x3c sda=13 scl=12");
    return true;
}

bool StatusDisplay::ready() { return displayReady; }

void StatusDisplay::show(const char* title, const String& first, const String& second,
                         const String& third, const String& fourth) {
    if (!displayReady) return;
    screen.clearDisplay();
    screen.setTextColor(SSD1306_WHITE);
    screen.setTextSize(1);
    screen.setTextWrap(false);
    line(0, String("GreenMind ") + title);
    screen.drawFastHLine(0, 11, 128, SSD1306_WHITE);
    line(17, first);
    line(29, second);
    line(41, third);
    line(53, fourth);
    screen.display();
}

namespace {
String elapsedLabel(uint32_t ms) {
    if (ms < 60000) return String(ms / 1000) + "s";
    if (ms < 3600000) return String(ms / 60000) + "min";
    return String(ms / 3600000) + "h";
}
String compactCount(uint32_t count) {
    if (count < 10000) return String(count);
    if (count < 1000000) return String(count / 1000) + "k";
    if (count < 1000000000) return String(count / 1000000) + "M";
    return String(count / 1000000000) + "G";
}
}

void StatusDisplay::showTelemetry(bool cloud, bool dual, bool wifi, bool clockReady,
                                  int rssi, const greenmind::UploadSnapshot& status,
                                  uint32_t lost, uint32_t now) {
    if (!displayReady) return;
    screen.clearDisplay();
    screen.setTextColor(SSD1306_WHITE);
    screen.setTextSize(1);
    screen.setTextWrap(false);
#ifdef GREENMIND_CLOUD_PRODUCTION
    const char* environment = "LIVE";
#else
    const char* environment = "TEST";
#endif
    const char* state = greenmind::uploadState(status, wifi, clockReady, now);
    const bool fresh = status.acknowledgements && now - status.lastAckMs < 3500;
    const char* route = cloud ? "CLOUD" : "GW";
    // The activity mark changes only with acknowledged uploads.
    String title = String(environment) + " " + route + " " + state;
    if (String(state) == "OK") title += status.acknowledgements % 2 ? " >" : " *";
    line(0, title);
    screen.drawFastHLine(0, 10, 128, SSD1306_WHITE);
    // Two-colour SSD1306: rows 0-15 yellow, 16-63 blue.
    line(17, wifi ? "WLAN " + String(rssi) + "dBm 380Hz" : "WLAN verbindet...");
    line(27, "Takt " + (fresh && status.acknowledgements > 1 ?
        (status.intervalMs < 10000 ? String(status.intervalMs / 1000.0f, 1) + "s" : elapsedLabel(status.intervalMs)) : String("--")) + " / Ziel 1s");
    line(37, status.acknowledgements ? "Letztes OK " +
        elapsedLabel(now - status.lastAckMs) + " her" : "Noch kein Empfang OK");
    line(47, !status.lastSucceeded && status.failures ?
        "HTTP " + String(status.result) + " Err " + compactCount(status.failures) :
        "OK " + compactCount(status.acknowledgements) + " | " + String(status.durationMs) + "ms");
    line(56, "Verlust " + compactCount(lost) + " E:" + compactCount(status.failures) +
        (dual ? " D" : ""));
    screen.display();
}
