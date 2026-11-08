#pragma once

// Included after submit()/OTA declarations. One real ADC conversion per timer
// event; callbacks never allocate, access the network or wait for an uploader.
// S3 continuous-ADC DMA has a 611 Hz lower bound in the pinned SDK. Legacy
// profiles use the 2500 us timer; durable profiles explicitly retain one in
// five untouched 2 kHz hardware conversions in Raw400DmaAcquisition.h.
struct Raw400Reading {
    uint64_t frame, originUs;
    uint16_t raw;
};
static greenmind::SpscQueue<Raw400Reading, 256> raw400Queue;
static greenmind::Raw400Clock raw400Clock;
static esp_timer_handle_t raw400Timer = nullptr;
static std::atomic<bool> raw400Enabled{false}, raw400CallbackActive{false};
static portMUX_TYPE raw400StatsLock = portMUX_INITIALIZER_UNLOCKED;
struct Raw400Stats {
    uint32_t conversions = 0, rail = 0, invalid = 0, queueDropped = 0;
    uint32_t duplicateWakeups = 0, maxConversionUs = 0, maxJitterUs = 0, maxCallbackUs = 0;
    uint64_t missingSlots = 0, maxIntervalUs = 0;
};
static Raw400Stats raw400Stats;
static uint64_t raw400WindowStart = 0, raw400PreviousRead = 0;
static std::atomic<uint32_t> raw400Stored{0};

static void raw400Callback(void*) {
    if (!raw400Enabled.load()) return;
    raw400CallbackActive.store(true);
    if (!raw400Enabled.load()) { raw400CallbackActive.store(false); return; }
    const uint64_t started = esp_timer_get_time();
    uint64_t frame = 0, missing = 0;
    uint32_t jitter = 0;
    if (!raw400Clock.admit(started, frame, missing, jitter)) {
        portENTER_CRITICAL(&raw400StatsLock);
        ++raw400Stats.duplicateWakeups;
        portEXIT_CRITICAL(&raw400StatsLock);
        raw400CallbackActive.store(false);
        return;
    }
    const int raw = adc1_get_raw(ADC1_CHANNEL_3);
    const uint32_t duration = static_cast<uint32_t>(esp_timer_get_time() - started);
    const bool valid = raw >= 0 && raw <= 4095;
    liveRawTap(raw, raw400Clock.originUs() + frame * greenmind::raw400IntervalUs);
    if (missing) timingDropped.fetch_add(static_cast<uint32_t>(missing));
    const bool queued = valid && raw400Queue.push(
        Raw400Reading{frame, raw400Clock.originUs(), static_cast<uint16_t>(raw)});
    if (!valid || !queued) timingDropped.fetch_add(1);
    portENTER_CRITICAL(&raw400StatsLock);
    if (!raw400WindowStart) raw400WindowStart = started;
    raw400Stats.missingSlots += missing;
    raw400Stats.maxConversionUs = max(raw400Stats.maxConversionUs, duration);
    raw400Stats.maxJitterUs = max(raw400Stats.maxJitterUs, jitter);
    if (raw400PreviousRead)
        raw400Stats.maxIntervalUs = max(raw400Stats.maxIntervalUs, started - raw400PreviousRead);
    raw400PreviousRead = started;
    if (valid) {
        ++raw400Stats.conversions;
        if (raw == 0 || raw == 4095) ++raw400Stats.rail;
        if (!queued) ++raw400Stats.queueDropped;
    } else ++raw400Stats.invalid;
    portEXIT_CRITICAL(&raw400StatsLock);
    portENTER_CRITICAL(&raw400StatsLock);
    raw400Stats.maxCallbackUs = max(raw400Stats.maxCallbackUs,
        static_cast<uint32_t>(esp_timer_get_time() - started));
    portEXIT_CRITICAL(&raw400StatsLock);
    adcHealthy.store(valid);
    adcFault.store(!valid);
    raw400CallbackActive.store(false);
}

static bool startRaw400() {
    if (!raw400Timer) {
        if (adc1_config_width(ADC_WIDTH_BIT_12) != ESP_OK ||
            adc1_config_channel_atten(ADC1_CHANNEL_3, ADC_ATTEN_DB_11) != ESP_OK) return false;
        esp_timer_create_args_t args{};
        args.callback = &raw400Callback;
        args.dispatch_method = ESP_TIMER_TASK;
        args.name = "gm-raw400";
        args.skip_unhandled_events = true;
        if (esp_timer_create(&args, &raw400Timer) != ESP_OK) return false;
    }
    raw400Enabled.store(true);
    if (esp_timer_start_periodic(raw400Timer, greenmind::raw400IntervalUs) == ESP_OK) return true;
    raw400Enabled.store(false);
    return false;
}

#ifdef GREENMIND_DIRECT_OTA
static void stopRaw400() {
    raw400Enabled.store(false);
    if (raw400Timer) esp_timer_stop(raw400Timer);
    while (raw400CallbackActive.load()) vTaskDelay(1);
    adcHealthy.store(false);
}
#endif

static void appendRaw400(const Raw400Reading& reading, uint64_t syncEpoch, uint64_t syncMono) {
#ifndef GREENMIND_DIRECT_ADC_SELFTEST
    if (!hasClock.load()) {
        if (!greenmind::sampleClockStart(syncEpoch, syncMono, reading.originUs, sessionStartUs)) {
            timingDropped.fetch_add(1);
            adcFault.store(true);
            return;
        }
        liveCaptureOrigin(reading.originUs, 400, 400);
        captureMetadata.sessionStartUs = sessionStartUs;
        captureMetadata.syncEpochUs = syncEpoch;
        hasClock.store(true, std::memory_order_release);
    }
#else
    (void)syncEpoch; (void)syncMono;
#endif
    int16_t pcm;
    uint16_t deciMv;
    if (!greenmind::encodeRaw400(reading.raw, pcm, deciMv)) return;
    if (acquisitionBlock.frames && reading.frame != acquisitionBlock.firstFrame + acquisitionBlock.frames) {
        // Preserve an explicit gap: a WAV run must never bridge missing samples.
        submit(acquisitionBlock);
        acquisitionBlock.frames = 0;
    }
    if (!acquisitionBlock.frames) acquisitionBlock.firstFrame = reading.frame;
    const uint16_t index = acquisitionBlock.frames++;
    acquisitionBlock.payload[index * 2] = static_cast<uint8_t>(pcm);
    acquisitionBlock.payload[index * 2 + 1] = static_cast<uint8_t>(uint16_t(pcm) >> 8);
    liveFrameTap(reading.frame, double(reading.raw) * 3300.0 / 4095.0);
    SignalDisplayStats::record(deciMv); // Display only; never fed back into PCM.
    raw400Stored.fetch_add(1);
    if (acquisitionBlock.frames == greenmind::raw400Hz) {
        submit(acquisitionBlock);
        acquisitionBlock.frames = 0;
    }
}

#ifdef GREENMIND_DIRECT_DURABLE_SPOOL
#include "Raw400DmaAcquisition.h"
#endif

static void acquireRaw400() {
#ifdef GREENMIND_DIRECT_DURABLE_SPOOL
    acquireRaw400Dma(); return;
#endif
    uint64_t epoch = 0, monotonic = 0;
#ifndef GREENMIND_DIRECT_ADC_SELFTEST
    while (!syncSnapshot(epoch, monotonic)) vTaskDelay(pdMS_TO_TICKS(50));
#endif
    captureMetadata.signalProfile = greenmind::SignalProfile::Ad8232Raw400V1;
    acquisitionBlock.sampleRate = greenmind::raw400Hz;
    acquisitionBlock.channels = 1;
    acquisitionBlock.sampleBits = 16;
    bool running = false;
    for (;;) {
#ifdef GREENMIND_DIRECT_OTA
        if (otaPauseRequested.load()) {
            if (running) { stopRaw400(); running = false; }
            Raw400Reading pending;
            while (raw400Queue.pop(pending)) appendRaw400(pending, epoch, monotonic);
            if (acquisitionBlock.frames) { submit(acquisitionBlock); acquisitionBlock.frames = 0; }
            otaAcquisitionPaused.store(true);
            while (otaPauseRequested.load()) vTaskDelay(pdMS_TO_TICKS(10));
            otaAcquisitionPaused.store(false);
            // Clock origin survives maintenance, exposing its actual missing slots.
        }
#endif
        if (!running) {
            if (!startRaw400()) {
                adcFault.store(true);
                adcRestarts.fetch_add(1);
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            running = true;
        }
        Raw400Reading reading;
        while (raw400Queue.pop(reading)) appendRaw400(reading, epoch, monotonic);
        vTaskDelay(1);
    }
}

static void printRaw400Stats() {
#ifdef GREENMIND_DIRECT_DURABLE_SPOOL
    printRaw400DmaStats(); return;
#endif
    portENTER_CRITICAL(&raw400StatsLock);
    const uint64_t now = esp_timer_get_time();
    const uint64_t windowStart = raw400WindowStart;
    const Raw400Stats stats = raw400Stats;
    const uint64_t window = raw400WindowStart ? now - raw400WindowStart : 0;
    raw400Stats = {};
    raw400WindowStart = now;
    portEXIT_CRITICAL(&raw400StatsLock);
    // Rail denominator is real valid ADC conversions in this measured window.
    // -1 means unavailable, never falsely report a missing measurement as zero.
    const double railPercent = stats.conversions ? 100.0 * stats.rail / stats.conversions : -1;
    Serial.printf("adc_profile=%s target_hz=400 interval_us=2500 raw_samples=%u raw_rail=%u rail_pct=%.3f window_us=%llu window_start_us=%llu window_end_us=%llu stored_frames=%u raw_invalid=%u scale_clipped=0 missed_slots=%llu duplicate_wakeups=%u queue_dropped=%u max_adc_us=%u max_callback_us=%u max_jitter_us=%u max_interval_us=%llu heap=%u min_heap=%u\n",
        greenmind::raw400Profile, stats.conversions, stats.rail, railPercent, window, windowStart, now,
        raw400Stored.exchange(0), stats.invalid, stats.missingSlots, stats.duplicateWakeups,
        stats.queueDropped, stats.maxConversionUs, stats.maxCallbackUs, stats.maxJitterUs, stats.maxIntervalUs,
        ESP.getFreeHeap(), ESP.getMinFreeHeap());
}
