#pragma once
#include "../../../direct_transport/Raw400DmaStride.h"

// DMA retains real conversions while flash operations stall cached CPU tasks.
// Five 1024-byte hardware frames cover 640 ms; the 16 KiB driver pool covers 2 s.
// Report delivery timing separately: it is not a measurement of ADC jitter.
static std::atomic<uint32_t> raw400DmaAll{0}, raw400DmaErrors{0}, raw400DmaMaxReadUs{0}, raw400DmaMaxDeliveryGapUs{0};
static uint64_t raw400DmaOrigin = 0;

static void acquireRaw400Dma() {
    uint64_t epoch = 0, monotonic = 0;
    while (!syncSnapshot(epoch, monotonic)) vTaskDelay(pdMS_TO_TICKS(50));
    captureMetadata.signalProfile = greenmind::SignalProfile::Ad8232Raw400V1;
    acquisitionBlock.sampleRate = 400; acquisitionBlock.channels = 1; acquisitionBlock.sampleBits = 16;
    if (!startContinuousAdc(1024, &raw400DmaOrigin)) {
        adcFault.store(true); raw400DmaErrors.fetch_add(1);
        Serial.println("raw400_dma_start_failed acquisition_paused=1");
        vTaskDelete(nullptr); return;
    }
    raw400DmaOrigin += 500; // Nominal first ADC conversion; absolute timing is unvalidated.
    raw400WindowStart = esp_timer_get_time();
    adcHealthy.store(true); adcFault.store(false);
    greenmind::Raw400DmaStride stride;
    greenmind::Raw400DmaDeliveryGuard delivery(raw400DmaOrigin - 500);
    uint64_t previousDelivery = raw400DmaOrigin - 500;
    alignas(4) uint8_t dma[1024];
    for (;;) {
        if (directFlashFault.load()) {
            if (acquisitionBlock.frames) { submit(acquisitionBlock); acquisitionBlock.frames = 0; }
            stopContinuousAdc(); adcFault.store(true);
            Serial.println("direct_flash_fault acquisition_paused=1 retained_data_unchanged=1");
            vTaskDelete(nullptr); return;
        }
        uint32_t bytes = 0;
        const uint64_t before = esp_timer_get_time();
        const esp_err_t result = adc_digi_read_bytes(dma, sizeof(dma), &bytes, 1000);
        const uint64_t completed = esp_timer_get_time();
        const uint32_t elapsed = completed - before;
        const uint32_t gap = completed - previousDelivery; previousDelivery = completed;
        if (gap > raw400DmaMaxDeliveryGapUs.load()) raw400DmaMaxDeliveryGapUs.store(gap);
        const uint32_t previous = raw400DmaMaxReadUs.load();
        if (elapsed > previous) raw400DmaMaxReadUs.store(elapsed);
        if (result != ESP_OK || !bytes || bytes % sizeof(adc_digi_output_data_t) ||
            !delivery.observe(completed)) {
            raw400DmaErrors.fetch_add(1); timingDropped.fetch_add(1);
            if (acquisitionBlock.frames) { submit(acquisitionBlock); acquisitionBlock.frames = 0; }
            stopContinuousAdc(); adcFault.store(true);
            Serial.println("raw400_dma_stream_failed acquisition_paused=1 missing_samples_unknown=1");
            vTaskDelete(nullptr); return; // Never invent samples or an exact lost-frame count.
        }
        raw400DmaAll.fetch_add(bytes / sizeof(adc_digi_output_data_t));
        for (uint32_t at = 0; at < bytes; at += sizeof(adc_digi_output_data_t)) {
            const auto* word = reinterpret_cast<const adc_digi_output_data_t*>(dma + at);
            if (word->type2.channel != ADC1_CHANNEL_3 || word->type2.unit != 0 || word->type2.data > 4095) {
                raw400DmaErrors.fetch_add(1); timingDropped.fetch_add(1);
                stopContinuousAdc(); adcFault.store(true);
                Serial.println("raw400_dma_identity_failed acquisition_paused=1");
                vTaskDelete(nullptr); return;
            }
            uint64_t frame = 0;
            if (!stride.take(frame)) continue;
            const uint16_t raw = word->type2.data;
            portENTER_CRITICAL(&raw400StatsLock);
            ++raw400Stats.conversions;
            if (raw == 0 || raw == 4095) ++raw400Stats.rail;
            portEXIT_CRITICAL(&raw400StatsLock);
            appendRaw400(Raw400Reading{frame, raw400DmaOrigin, raw}, epoch, monotonic);
            liveRawTap(raw, raw400DmaOrigin + frame * 2500ULL);
        }
        vTaskDelay(1);
    }
}

static void printRaw400DmaStats() {
    portENTER_CRITICAL(&raw400StatsLock);
    const uint64_t now = esp_timer_get_time(), start = raw400WindowStart;
    const Raw400Stats stats = raw400Stats;
    raw400Stats = {}; raw400WindowStart = now;
    portEXIT_CRITICAL(&raw400StatsLock);
    const double rail = stats.conversions ? 100.0 * stats.rail / stats.conversions : -1;
    Serial.printf("adc_dma_profile=ad8232-raw400-v1 target_hz=400 hardware_hz=2000 stride=5 averaged=0 raw_samples=%u raw_dma_samples=%u raw_rail=%u rail_pct=%.3f window_us=%llu window_start_us=%llu window_end_us=%llu stored_frames=%u dma_errors=%u max_read_wait_us=%u max_delivery_gap_us=%u sampling_jitter_measured=0 absolute_timing_validated=0 heap=%u min_heap=%u\n",
        stats.conversions, raw400DmaAll.exchange(0), stats.rail, rail,
        start ? now - start : 0, start, now, raw400Stored.exchange(0),
        raw400DmaErrors.load(), raw400DmaMaxReadUs.exchange(0), raw400DmaMaxDeliveryGapUs.exchange(0), ESP.getFreeHeap(), ESP.getMinFreeHeap());
}
