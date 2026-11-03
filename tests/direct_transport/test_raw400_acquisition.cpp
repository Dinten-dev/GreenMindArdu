// Execute the actual acquisition module with a deterministic ADC/timer boundary.
// These are software contract tests, not a hardware timing measurement.
#include "../../direct_transport/Ad8232Raw400.h"
#include "../../direct_transport/CaptureClock.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

using std::max;
using greenmind::SampleBlock;
using esp_timer_handle_t = void*;
using portMUX_TYPE = int;
constexpr int portMUX_INITIALIZER_UNLOCKED = 0;
constexpr int ADC1_CHANNEL_3 = 3, ADC_WIDTH_BIT_12 = 12, ADC_ATTEN_DB_11 = 11;
constexpr int ESP_OK = 0, ESP_TIMER_TASK = 0;
static uint64_t testTime = 1000000, timerPeriod = 0;
static int testAdc = 0;
static uint32_t adcCalls = 0;
static void (*timerCallback)(void*) = nullptr;
struct StopTask {};
struct esp_timer_create_args_t {
    void (*callback)(void*);
    void* arg;
    int dispatch_method;
    const char* name;
    bool skip_unhandled_events;
};
inline void portENTER_CRITICAL(int*) {}
inline void portEXIT_CRITICAL(int*) {}
inline int pdMS_TO_TICKS(int value) { return value; }
inline void vTaskDelay(int) { throw StopTask{}; }
inline uint64_t esp_timer_get_time() { return testTime; }
inline int adc1_get_raw(int channel) {
    assert(channel == ADC1_CHANNEL_3);
    ++adcCalls;
    testTime += 25;
    return testAdc;
}
inline int adc1_config_width(int width) { assert(width == 12); return ESP_OK; }
inline int adc1_config_channel_atten(int channel, int attenuation) {
    assert(channel == ADC1_CHANNEL_3 && attenuation == ADC_ATTEN_DB_11);
    return ESP_OK;
}
inline int esp_timer_create(const esp_timer_create_args_t* args, esp_timer_handle_t* handle) {
    assert(args->skip_unhandled_events && args->dispatch_method == ESP_TIMER_TASK);
    timerCallback = args->callback;
    *handle = reinterpret_cast<void*>(1);
    return ESP_OK;
}
inline int esp_timer_start_periodic(esp_timer_handle_t, uint64_t period) {
    timerPeriod = period;
    return ESP_OK;
}
struct {
    uint32_t getFreeHeap() { return 110000; }
    uint32_t getMinFreeHeap() { return 100000; }
} ESP;
struct {
    std::string last;
    template<class... Args> void printf(const char* format, Args... args) {
        char buffer[2048];
        std::snprintf(buffer, sizeof(buffer), format, args...);
        last = buffer;
    }
} Serial;
static std::atomic<bool> hasClock{false}, adcHealthy{false}, adcFault{false};
static std::atomic<uint32_t> timingDropped{0}, adcRestarts{0};
static uint64_t sessionStartUs = 0;
static greenmind::CaptureMetadata captureMetadata;
static SampleBlock acquisitionBlock;
static std::vector<SampleBlock> submitted;
static void submit(SampleBlock& block) {
    block.capture = captureMetadata;
    assert(block.valid());
    submitted.push_back(block);
}
static bool syncSnapshot(uint64_t& epoch, uint64_t& mono) {
    epoch = 1800000000000000ULL;
    mono = 900000;
    return true;
}
namespace SignalDisplayStats { inline void record(uint16_t) {} }

#include "../../examples/direct_sensor/src/Raw400Acquisition.h"

int main() {
    try { acquireRaw400(); } catch (StopTask&) {}
    assert(timerPeriod == 2500 && timerCallback);
    assert(acquisitionBlock.sampleRate == 400);
    assert(captureMetadata.signalProfile == greenmind::SignalProfile::Ad8232Raw400V1);
    for (uint64_t i = 0; i < 400; ++i) {
        testTime = 1000000 + i * 2500;
        testAdc = i == 399 ? 4095 : int(i * 7);
        timerCallback(nullptr);
        Raw400Reading reading;
        assert(raw400Queue.pop(reading));
        appendRaw400(reading, 1800000000000000ULL, 900000);
    }
    assert(submitted.size() == 1 && submitted[0].frames == 400);
    assert(submitted[0].firstFrame == 0 && submitted[0].sampleRate == 400);
    assert(submitted[0].capture.sessionStartUs == 1800000000100000ULL);
    for (uint16_t i = 0; i < 400; ++i) {
        int16_t expected;
        uint16_t display;
        assert(greenmind::encodeRaw400(i == 399 ? 4095 : i * 7, expected, display));
        assert(submitted[0].payload[i * 2] == uint8_t(expected));
        assert(submitted[0].payload[i * 2 + 1] == uint8_t(uint16_t(expected) >> 8));
    }
    // A catch-up callback cannot create a second reading in the same slot.
    timerCallback(nullptr);
    assert(adcCalls == 400 && raw400Stats.duplicateWakeups == 1);
    testTime = 2000000;
    printRaw400Stats();
    assert(Serial.last.find("raw_samples=400 raw_rail=2") != std::string::npos);
    assert(Serial.last.find("window_us=1000000") != std::string::npos);
    assert(Serial.last.find("scale_clipped=0") != std::string::npos);

    // Invalid conversion and missed timer slots are gaps, never repeated PCM.
    testAdc = -1;
    timerCallback(nullptr); // Frame 400 invalid.
    assert(!adcHealthy.load() && adcFault.load());
    testTime = 2010000; // Frame 404: three timer slots were missed.
    testAdc = 4095;
    timerCallback(nullptr);
    Raw400Reading reading;
    assert(raw400Queue.pop(reading) && reading.frame == 404);
    appendRaw400(reading, 1800000000000000ULL, 900000);
    testTime = 2017500; // Frame 407: partial frame 404 must be flushed separately.
    timerCallback(nullptr);
    assert(raw400Queue.pop(reading));
    appendRaw400(reading, 1800000000000000ULL, 900000);
    assert(submitted.size() == 2);
    assert(submitted[1].firstFrame == 404 && submitted[1].frames == 1);
    assert(acquisitionBlock.firstFrame == 407 && acquisitionBlock.frames == 1);
    assert(raw400Stats.invalid == 1 && raw400Stats.missingSlots == 5);
    assert(timingDropped.load() == 6);

    // Bounded raw ring does not overwrite older readings during a stalled consumer.
    for (uint64_t i = 408; i < 668; ++i) {
        testTime = 1000000 + i * 2500;
        timerCallback(nullptr);
    }
    assert(raw400Stats.queueDropped == 4);
    assert(raw400Queue.pop(reading) && reading.frame == 408);
    std::cout << "PASS: real acquisition callback, raw PCM, diagnostic rails, missed slots and bounded ring\n";
}
