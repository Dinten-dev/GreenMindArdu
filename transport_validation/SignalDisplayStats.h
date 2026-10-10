#pragma once
#include <Arduino.h>

// Display-only statistics of the existing ADC reads. One sampling-task writer.
// Publish a complete second together; rendering never holds the sampling lock.
namespace SignalDisplayStats {
struct Snapshot {
    uint16_t meanDeciMv = 0, minDeciMv = 0, maxDeciMv = 0;
    uint32_t completedAtMs = 0;
    bool available = false;
    bool fresh(uint32_t now) const { return available && now - completedAtMs < 2000; }
};
static portMUX_TYPE snapshotMux = portMUX_INITIALIZER_UNLOCKED;
static Snapshot completed;
static uint32_t sum = 0;
static uint16_t count = 0, minimum = UINT16_MAX, maximum = 0;
inline void record(uint16_t deciMv) {
    sum += deciMv;
    if (deciMv < minimum) minimum = deciMv;
    if (deciMv > maximum) maximum = deciMv;
    if (++count != 380) return;
    Snapshot next;
    next.meanDeciMv = static_cast<uint16_t>(sum / count);
    next.minDeciMv = minimum; next.maxDeciMv = maximum;
    next.completedAtMs = millis(); next.available = true;
    portENTER_CRITICAL(&snapshotMux);
    completed = next;
    portEXIT_CRITICAL(&snapshotMux);
    sum = 0; count = 0; minimum = UINT16_MAX; maximum = 0;
}
inline Snapshot read() {
    portENTER_CRITICAL(&snapshotMux);
    Snapshot result = completed;
    portEXIT_CRITICAL(&snapshotMux);
    return result;
}
}
