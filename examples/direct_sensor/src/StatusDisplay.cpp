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
