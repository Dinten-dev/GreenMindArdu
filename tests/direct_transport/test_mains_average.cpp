#include "../../transport_validation/MainsAverage20ms.h"
#include <cassert>
#include <cmath>
#include <iostream>

static double amplitude(double hz) {
    greenmind::MainsAverage20ms filter;
    double minimum = 4095, maximum = 0;
    for (unsigned n = 0; n < 8000; ++n) {
        const auto raw = static_cast<uint16_t>(std::lround(
            2048 + 1000 * std::sin(2 * std::acos(-1.) * hz * n / 2000)));
        if (filter.add(raw) && n >= 2000) {
            minimum = std::fmin(minimum, filter.mean());
            maximum = std::fmax(maximum, filter.mean());
        }
    }
    return (maximum - minimum) / 2;
}

int main() {
    greenmind::MainsAverage20ms filter;
    for (unsigned n = 0; n < 39; ++n) assert(!filter.add(4095));
    assert(filter.add(4095) && filter.mean() == 4095);
    for (unsigned n = 0; n < 40; ++n) filter.add(0);
    assert(filter.mean() == 0);
    filter.reset();
    for (unsigned n = 0; n < 39; ++n) assert(!filter.add(100));
    assert(filter.add(100) && filter.mean() == 100);
    // A full new window is mandatory after a real capture gap/reset.
    filter.reset();
    assert(!filter.add(200));
    assert(greenmind::MainsAverage20ms::groupDelayUs == 9750);
    assert(amplitude(1) > 998);
    assert(amplitude(10) > 930 && amplitude(10) < 940);
    assert(amplitude(20) > 750 && amplitude(20) < 765);
    for (double hz : {50., 100., 150.}) assert(amplitude(hz) < .1);
    // A drifted mains frequency is attenuated, not falsely claimed zero.
    assert(amplitude(49) > 10 && amplitude(49) < 30);
    std::cout << "PASS: real-window warmup/reset, rails, DC/1/10/20Hz response, 50/100/150Hz rejection\n";
}
