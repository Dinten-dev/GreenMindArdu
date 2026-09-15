#pragma once

#include <Arduino.h>

namespace StatusDisplay {
bool init();
bool ready();
void show(const char* title, const String& first, const String& second,
          const String& third, const String& fourth);
}
