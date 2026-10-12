#include "../../direct_transport/PcmSpool.h"
#include <cassert>
#include <iostream>
#include <thread>
#include <vector>

using namespace greenmind;

static SampleBlock sample(uint64_t second) {
    SampleBlock block;
    block.sequence = second;
    block.firstFrame = second * 380;
    block.frames = 380;
    for (size_t i = 0; i < block.byteCount(); ++i)
        block.payload[i] = uint8_t(second + i);
    return block;
}

static void verify(const SampleBlock& block, uint64_t second) {
    assert(block.sequence == second && block.firstFrame == second * 380);
    assert(block.frames == 380 && block.sampleRate == 380);
    assert(block.channels == 1 && block.sampleBits == 16);
    for (size_t i = 0; i < block.byteCount(); ++i)
        assert(block.payload[i] == uint8_t(second + i));
}

int main() {
    std::vector<uint8_t> memory(128 * 1024);
    PcmSpool spool;
    assert(!spool.begin(nullptr, memory.size()));
    assert(!spool.begin(memory.data(), 12345));
    assert(spool.begin(memory.data(), memory.size()));
    assert(!spool.begin(memory.data(), memory.size()));
    assert(spool.capacitySeconds380() >= 120);
    SampleBlock read;
    uint64_t next = 0;
    // Server outage crosses the 600s boundary; identical retry retained 120s.
    for (uint64_t second = 0; second < 1400; ++second) {
        auto block = sample(second);
        assert(spool.push(block));
        if (second >= 570 && second < 690) {
            assert(spool.peek(read));
            verify(read, next);
            assert(!spool.acknowledge(next + 1));
            continue;
        }
        while (spool.peek(read)) {
            verify(read, next);
            assert(spool.acknowledge(next++));
        }
    }
    assert(next == 1400 && spool.used() == 0);
    assert(!spool.acknowledge(next));
    // Full ring never overwrites unacknowledged data.
    uint64_t queued = 0;
    while (spool.push(sample(queued))) ++queued;
    assert(queued == spool.capacitySeconds380());
    for (uint64_t i = 0; i < queued; ++i) {
        assert(spool.peek(read));
        verify(read, i);
        assert(spool.acknowledge(i));
    }
    // Tiny ring forces wraparound and concurrent producer/consumer ownership.
    std::vector<uint8_t> small(8192);
    PcmSpool concurrent;
    assert(concurrent.begin(small.data(), small.size()));
    std::thread consumer([&] {
        SampleBlock current;
        for (uint64_t i = 0; i < 100000; ++i) {
            while (!concurrent.peek(current)) std::this_thread::yield();
            verify(current, i);
            assert(concurrent.acknowledge(i));
        }
    });
    for (uint64_t i = 0; i < 100000; ++i) {
        const auto block = sample(i);
        while (!concurrent.push(block)) std::this_thread::yield();
    }
    consumer.join();
    assert(concurrent.used() == 0);
    // Preserve the existing variable-format Direct transport boundary.
    SampleBlock multichannel;
    multichannel.channels = 4;
    multichannel.sampleBits = 24;
    multichannel.sampleRate = 2000;
    multichannel.frames = 500;
    multichannel.sequence = 123;
    for (size_t i = 0; i < multichannel.byteCount(); ++i)
        multichannel.payload[i] = uint8_t(i);
    assert(concurrent.push(multichannel));
    assert(concurrent.peek(read));
    assert(read.byteCount() == 6000 && read.channels == 4 && read.sampleBits == 24);
    assert(std::memcmp(read.payload, multichannel.payload, 6000) == 0);
    assert(concurrent.acknowledge(123));
    // Exercise actual uint32 byte-counter rollover (over 4 GiB transported).
    for (uint64_t i = 0; i < 720000; ++i) {
        multichannel.sequence = i;
        assert(concurrent.push(multichannel));
        assert(concurrent.peek(read) && read.sequence == i);
        assert(std::memcmp(read.payload, multichannel.payload, 6000) == 0);
        assert(concurrent.acknowledge(i));
    }
    assert(concurrent.used() == 0);
    multichannel.frames = 501;
    assert(!concurrent.push(multichannel));
    std::cout << "PASS: 120s outage, 1400s continuous PCM, exact retries, full ring, "
                 "100000 concurrent records, uint32 rollover and PCM24\n";
}
