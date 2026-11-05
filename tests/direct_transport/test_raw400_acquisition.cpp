#include "Raw400Harness.h"

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
