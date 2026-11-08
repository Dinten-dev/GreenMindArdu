#include "../../direct_transport/Raw400DmaStride.h"
#include "../../direct_transport/Ad8232Raw400.h"
#include <cassert>
#include <iostream>
int main() {
    greenmind::Raw400DmaStride stride;
    uint64_t kept = 0, frame = 0;
    // Irregular DMA deliveries never reset the phase or alter raw values.
    const unsigned batch[] = {17, 256, 1, 1024, 31, 93};
    uint64_t input = 0;
    for (unsigned turn = 0; input < 200000; ++turn) {
        for (unsigned i = 0; i < batch[turn % 6] && input < 200000; ++i, ++input) {
            const uint16_t raw = input % 4096;
            if (!stride.take(frame)) continue;
            assert(input % 5 == 0 && frame == input / 5 && frame == kept++);
            int16_t pcm; uint16_t deci;
            assert(greenmind::encodeRaw400(raw, pcm, deci));
            assert(pcm == greenmind::legacyPcm(double(raw) * 3300.0 / 4095.0));
        }
    }
    assert(kept == 40000 && stride.conversions() == 200000);
    greenmind::Raw400DmaDeliveryGuard guard(100);
    uint64_t now = 100;
    for (unsigned i = 0; i < 10000; ++i) { now += 128000 + (i % 7) * 1000; assert(guard.observe(now)); }
    assert(!guard.observe(now - 1));
    assert(!guard.observe(now + 640000)); // DMA capacity cannot hide a prolonged CPU stall.
    std::cout << "DMA stride preserves every fifth real raw value across batch boundaries\n";
}
