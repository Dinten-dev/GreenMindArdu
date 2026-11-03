#include "Raw400Harness.h"

int main() {
    try { acquireRaw400(); } catch (StopTask&) {}
    for (uint64_t frame = 0; frame < 8000; ++frame) {
        testTime = 1000000 + frame * 2500;
        testAdc = frame % 2 ? 4095 : 0;
        if (frame == 4000) {
            liveRails.reset(400);
            liveAverage.reset(400);
            liveUntil.store(millis() + 2000);
        }
        timerCallback(nullptr);
        Raw400Reading reading;
        assert(raw400Queue.pop(reading));
        appendRaw400(reading, 1800000000000000ULL, 900000);
    }
    // A full, undrained preview ring loses previews, never raw data or ACK ownership.
    assert(liveDropped.load() > 0);
    assert(submitted.size() == 20 && timingDropped.load() == 0);
    assert(raw400Stats.queueDropped == 0 && adcCalls == 8000);
    for (uint64_t second = 0; second < submitted.size(); ++second) {
        const auto& block = submitted[second];
        assert(block.firstFrame == second * 400 && block.frames == 400);
        for (uint16_t i = 0; i < 400; ++i) {
            assert(block.payload[i * 2] == (i % 2 ? 255 : 0));
            assert(block.payload[i * 2 + 1] == (i % 2 ? 127 : 0));
        }
    }
    const auto rail = liveRails.snapshot();
    assert(rail.samples == 800 && rail.railed == 800); // Only the two active seconds.
    assert(!greenmind::liveDemand(millis(), liveUntil.load()));
    greenmind::LivePoint point;
    uint32_t points = 0;
    while (livePoints.pop(point)) { assert(point.mv == 1650); ++points; }
    assert(points == 16);
    printRaw400Stats();
    assert(Serial.last.find("adc_profile=ad8232-raw400-v1") != std::string::npos);
    std::cout << "PASS: actual live taps preserve every raw byte, bounded preview loss, automatic expiry\n";
}
