#pragma once

#include <Arduino.h>
#include "UploadStatus.h"

namespace StatusDisplay {
bool init();
bool ready();
void showTelemetry(bool cloud, bool dual, bool wifi, bool clockReady, int rssi,
                   const greenmind::UploadSnapshot& status, uint32_t lost, uint32_t now,
                   uint16_t meanDeciMv, uint16_t minDeciMv, uint16_t maxDeciMv,
                   bool sampleFresh, const String& sensorId,
                   uint32_t spoolUsed, uint32_t spoolCapacity, bool psram);
void show(const char* title, const String& first, const String& second,
          const String& third, const String& fourth);
}
