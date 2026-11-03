#pragma once
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

inline uint32_t millis() { return static_cast<uint32_t>(testTime / 1000); }
#include "../../examples/direct_sensor/src/DemandLive.h"
#include "../../examples/direct_sensor/src/Raw400Acquisition.h"
