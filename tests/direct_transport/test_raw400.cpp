#include "../../direct_transport/Ad8232Raw400.h"
#include "../../direct_transport/PcmSpool.h"
#include <cassert>
#include <fstream>
#include <iostream>
#include <vector>

int main(int argc, char** argv) {
    using namespace greenmind;
    int16_t previous = -1, pcm = -1;
    uint16_t mv = 0;
    std::ofstream output;
    if (argc == 2) output.open(argv[1], std::ios::binary);
    for (uint16_t raw = 0; raw <= 4095; ++raw) {
        assert(encodeRaw400(raw, pcm, mv));
        assert(pcm > previous); // Every ADC code survives: no clipping/averaging.
        assert(pcm == legacyPcm(double(raw) * 3300.0 / 4095.0));
        if (raw == 0) assert(pcm == 0 && mv == 0);
        if (raw == 4095) assert(pcm == 32767 && mv == 33000);
        previous = pcm;
        if (output.is_open()) { output.put(char(pcm)); output.put(char(uint16_t(pcm) >> 8)); }
    }
    assert(!encodeRaw400(4096, pcm, mv));
    assert(!encodeRaw400(65535, pcm, mv));
    // HPK's mV*10 PCM would clip above 3276.7 mV. GreenMind retains these codes.
    assert(encodeRaw400(4090, pcm, mv) && pcm < 32767 && mv > 32767);

    Raw400Clock clock;
    uint64_t frame, missing;
    uint32_t jitter;
    constexpr uint64_t origin = 0x100000000ULL - 1000; // Cross 32-bit micros wrap.
    for (uint64_t i = 0; i < 400000; ++i) {
        assert(clock.admit(origin + i * 2500, frame, missing, jitter));
        assert(frame == i && missing == 0 && jitter == 0);
        assert(!clock.admit(origin + i * 2500 + 20, frame, missing, jitter));
    }
    assert(clock.admit(origin + 400003ULL * 2500 + 100, frame, missing, jitter));
    assert(frame == 400003 && missing == 3 && jitter == 100);
    assert(!clock.admit(origin + 400003ULL * 2500 + 120, frame, missing, jitter));
    assert(!clock.admit(origin - 1, frame, missing, jitter));
    // A maintenance pause preserves its gap instead of restarting sample indices.
    assert(clock.admit(origin + 500000ULL * 2500, frame, missing, jitter));
    assert(frame == 500000 && missing == 99996);

    std::vector<uint8_t> memory(128 * 1024);
    PcmSpool spool;
    assert(spool.begin(memory.data(), memory.size()));
    assert(spool.capacitySeconds(400) >= 120);
    assert(spool.capacitySeconds(400) < spool.capacitySeconds380());
    SampleBlock block, read;
    block.sampleRate = block.frames = 400;
    block.capture.signalProfile = SignalProfile::Ad8232Raw400V1;
    block.capture.sessionStartUs = 1800000000000000ULL;
    std::strcpy(block.capture.sessionId, "12345678-1234-4234-8234-123456789abc");
    block.payload[0] = 0; block.payload[1] = 0;
    block.payload[798] = 255; block.payload[799] = 127;
    uint32_t seconds = 0;
    while (spool.push(block)) { block.sequence = ++seconds; block.firstFrame += 400; }
    assert(seconds == spool.capacitySeconds(400));
    for (uint32_t i = 0; i < seconds; ++i) {
        assert(spool.peek(read) && read.capture.signalProfile == SignalProfile::Ad8232Raw400V1);
        assert(read.frames == 400 && read.sampleRate == 400 && read.firstFrame == i * 400);
        assert(read.payload[798] == 255 && read.payload[799] == 127);
        assert(!spool.acknowledge(i + 1)); // Exact ACK retains raw bytes through retry.
        assert(spool.acknowledge(i));
    }
    assert(spool.used() == 0);
    block.sampleRate = 380;
    assert(!block.valid()); // Never send a raw400 profile with a legacy clock.
    std::cout << "PASS: 4096 distinct codes, both rails, nominal 400 Hz, gaps, profile and bounded spool\n";
}
