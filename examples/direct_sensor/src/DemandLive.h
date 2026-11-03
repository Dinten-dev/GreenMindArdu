#pragma once
#include "../../../direct_transport/LivePreviewCore.h"

static std::atomic<uint32_t> liveUntil{0}, liveDropped{0};
static std::atomic<uint32_t> liveHttpMaximumMs{0}, liveHttpErrors{0}, livePackets{0};
static portMUX_TYPE liveLock = portMUX_INITIALIZER_UNLOCKED;
static greenmind::LiveAverager liveAverage;
static greenmind::LiveRailWindow liveRails;
static greenmind::SpscQueue<greenmind::LivePoint, greenmind::liveQueuePoints> livePoints;
static uint64_t liveOriginMonotonic = 0;
static uint16_t liveSourceRate = 400, liveRawRate = 400;

// Acquisition only: fixed-size arithmetic/memory, no allocation or network.
static void liveRawTap(int raw, uint64_t monotonic) {
    if (!greenmind::liveDemand(millis(), liveUntil.load()) || !hasClock.load()) return;
    portENTER_CRITICAL(&liveLock);
    if (monotonic >= liveOriginMonotonic)
        liveRails.add((monotonic - liveOriginMonotonic) / (1000000 / liveRawRate), raw);
    portEXIT_CRITICAL(&liveLock);
}
static void liveFrameTap(uint64_t frame, double mv) {
    if (!greenmind::liveDemand(millis(), liveUntil.load())) return;
    portENTER_CRITICAL(&liveLock);
    greenmind::LivePoint point;
    if (liveAverage.add(frame, mv, point) && !livePoints.push(point)) liveDropped.fetch_add(1);
    portEXIT_CRITICAL(&liveLock);
}
static void liveCaptureOrigin(uint64_t monotonic, uint16_t sourceRate, uint16_t rawRate) {
    portENTER_CRITICAL(&liveLock);
    liveOriginMonotonic = monotonic;
    liveSourceRate = sourceRate;
    liveRawRate = rawRate;
    portEXIT_CRITICAL(&liveLock);
}
