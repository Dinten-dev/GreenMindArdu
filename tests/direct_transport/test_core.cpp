#include "../../direct_transport/DirectCore.h"
#include "../../examples/direct_sensor/src/PairingCode.h"
#include <cassert>
#include <iostream>
#include <fstream>
#include <thread>

int main(int argc, char** argv) {
    using namespace greenmind;
    assert(validPairingCode("A2BC34", 6));
    assert(validPairingCode("AB23CD45", 8));
    assert(validPairingCode("123456", 6));
    assert(validPairingCode("ABCDEFGH", 8));
    assert(!validPairingCode(nullptr, 6));
    assert(!validPairingCode("", 0));
    assert(!validPairingCode("ABCDE", 5));
    assert(!validPairingCode("ABCDEFG", 7));
    assert(!validPairingCode("ABCDEFGHI", 9));
    assert(!validPairingCode("AB 234", 6));
    assert(!validPairingCode("AB-234", 6));
    assert(!validPairingCode("ABC\0EF", 6));
    // Exercise every byte at every accepted position, including digit bitmasks
    // and non-ASCII input that must not depend on locale or signed-char ctype.
    for (std::size_t length : {6u, 8u}) {
        std::string candidate(length, 'A');
        for (std::size_t pos = 0; pos < length; ++pos) {
            for (int value = 0; value < 256; ++value) {
                candidate[pos] = static_cast<char>(value);
                const bool expected = (value >= 'A' && value <= 'Z') ||
                                      (value >= '0' && value <= '9');
                assert(validPairingCode(candidate.data(), length) == expected);
            }
            candidate[pos] = 'A';
        }
    }
    assert(parseMode(nullptr) == Mode::Gateway);
    assert(parseMode("") == Mode::Gateway);
    assert(parseMode("DIRECT") == Mode::Direct);
    assert(parseMode("DUAL") == Mode::Dual);
    assert(parseMode("invalid") == Mode::Invalid);
    uint8_t bytes[3];
    assert(pack24(-8388608, bytes) && bytes[0] == 0 && bytes[1] == 0 && bytes[2] == 0x80);
    assert(pack24(8388607, bytes) && bytes[0] == 0xff && bytes[1] == 0xff && bytes[2] == 0x7f);
    assert(pack24(-1, bytes) && bytes[0] == 0xff && bytes[2] == 0xff);
    assert(pack24(65537, bytes) && bytes[0] == 1 && bytes[1] == 0 && bytes[2] == 1);
    assert(!pack24(8388608, bytes));
    assert(legacyPcm(-1) == 0 && legacyPcm(4000) == 32767);
    assert(legacyPcm(1650) == 16383);
    if (argc == 2) {
        std::ofstream output(argv[1], std::ios::binary);
        for (int value = 0; value <= 33000; ++value) {
            uint16_t pcm = static_cast<uint16_t>(legacyPcm(value * 0.1));
            output.put(static_cast<char>(pcm));
            output.put(static_cast<char>(pcm >> 8));
        }
        assert(output.good());
    }
    SpscQueue<SampleBlock, 2> queue;
    SampleBlock source, target;
    source.frames = 1;
    source.payload[0] = 42;
    assert(queue.push(source));
    source.payload[0] = 99;
    assert(queue.push(source));
    assert(!queue.push(source));
    assert(queue.pop(target) && target.payload[0] == 42);
    assert(queue.pop(target) && target.payload[0] == 99);
    assert(!queue.pop(target));
    SpscQueue<SampleBlock, 4> stressed;
    std::thread consumer([&] {
        SampleBlock block;
        for (uint64_t i = 0; i < 10000; ++i) {
            while (!stressed.pop(block)) std::this_thread::yield();
            assert(block.sequence == i && block.payload[5999] == uint8_t(i));
        }
    });
    for (uint64_t i = 0; i < 10000; ++i) {
        source.sequence = i;
        source.payload[5999] = uint8_t(i);
        while (!stressed.push(source)) std::this_thread::yield();
    }
    consumer.join();
    std::cout << "PASS: pairing codes, modes, PCM16/24, bounded ownership, 10000 concurrent blocks\n";
}
