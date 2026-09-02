#pragma once

#include <Arduino.h>

static constexpr uint16_t GREENMIND_SAMPLE_RATE = 380;
static constexpr uint16_t GREENMIND_BATCH_SAMPLES = 380;
static constexpr uint16_t GREENMIND_PROTOCOL_VERSION = 3;

struct QualityCounts {
    uint16_t valid;
    uint16_t leadOff;
    uint16_t railHigh;
    uint16_t railLow;
    uint16_t jump;
    uint16_t recovery;
};

struct MeasurementBatch {
    uint32_t bootId;
    uint32_t sequence;
    uint32_t uptimeMs;
    uint32_t droppedSamplesTotal;
    uint64_t capturedAtEpochMs;
    uint16_t sampleRate;
    uint16_t sampleCount;
    uint16_t protocolVersion;
    char firmwareVersion[16];
    char calibrationVersion[32];
    QualityCounts quality;
    uint16_t samplesDeciMv[GREENMIND_BATCH_SAMPLES];
};
