#include "../../direct_transport/LivePreviewCore.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <iostream>

int main() {
    using namespace greenmind;
    for (uint16_t rate : {400, 380}) {
        LiveAverager average;
        average.reset(rate);
        LivePoint point;
        uint32_t outputs = 0, total = 0;
        // 20 minutes: rational 380 Hz phase never drifts to 12.66 Hz.
        for (uint64_t frame = 0; frame < rate * 1200ULL; ++frame) {
            if (average.add(frame, 1650.0, point)) {
                assert(point.mv == 1650.0);
                assert(point.samples == 32 || (rate == 380 && (point.samples == 30 || point.samples == 31)));
                total += point.samples; ++outputs;
            }
        }
        assert(outputs == 15000 && total == uint32_t(rate) * 1200);
    }
    LiveAverager average;
    average.reset(400);
    LivePoint point;
    double maxHum = 0;
    for (uint64_t frame = 0; frame < 4000; ++frame) {
        const double raw = 1650 + 1000 * std::sin(2 * 3.14159265358979323846 * 50 * frame / 400);
        if (average.add(frame, raw, point)) maxHum = std::max(maxHum, std::abs(point.mv - 1650));
    }
    assert(maxHum < 1e-8); // 32 samples cover four exact nominal mains periods.
    average.reset(400);
    for (uint64_t i = 0; i < 31; ++i) assert(!average.add(i, 0, point));
    assert(!average.add(1000, 3300, point)); // Gap discards the incomplete old bin.
    for (uint64_t i = 1001; i < 1031; ++i) assert(!average.add(i, 3300, point));
    assert(average.add(1031, 3300, point) && point.mv == 3300);

    for (uint16_t rawRate : {400, 2000}) {
        LiveRailWindow rail;
        assert(rail.reset(rawRate));
        for (uint64_t tick = 0; tick < rawRate * 20ULL; ++tick)
            rail.add(tick, tick % 5 ? 4095 : 1650);
        auto snapshot = rail.snapshot();
        assert(snapshot.samples == uint32_t(rawRate) * 10 && snapshot.railed == uint32_t(rawRate) * 8);
        assert(snapshot.endTick - snapshot.firstTick == uint32_t(rawRate) * 10);
        rail.add(rawRate * 30ULL, -1); // Long gap: no fabricated zero-rail observations.
        snapshot = rail.snapshot();
        assert(snapshot.samples == 0 && snapshot.railed == 0);
        rail.add(rawRate * 30ULL + 1, 0);
        snapshot = rail.snapshot();
        assert(snapshot.samples == 1 && snapshot.railed == 1);
    }
    assert(liveDemand(0xfffffff0, 100));
    assert(liveDemand(99, 100));
    assert(!liveDemand(100, 100));
    assert(!liveDemand(101, 100));
    assert(!liveDemand(100, 0));
    std::cout << "PASS: exact 12.5 Hz, 400/380 profiles, independent rail window, gaps and lease expiry\n";
}
