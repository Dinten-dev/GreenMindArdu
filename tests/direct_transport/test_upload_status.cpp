#include "../../examples/direct_sensor/src/UploadStatus.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <limits>

int main() {
    using namespace greenmind;
    UploadStatus direct, gateway;
    auto state = [&](const char* expected, uint32_t now, bool wifi = true, bool clock = true) {
        assert(std::strcmp(uploadState(direct.snapshot(), wifi, clock, now), expected) == 0);
    };
    state("WARTE OK", 1000);
    state("WLAN FEHLT", 1000, false);
    state("WARTE UHR", 1000, true, false);
    direct.record(false, 201, 1000, 80); // HTTP success with invalid identity/hash is not ACK.
    state("SENDEFEHLER", 1000);
    assert(direct.snapshot().acknowledgements == 0);
    direct.record(true, 201, 2000, 70);
    state("OK", 2000);
    assert(direct.snapshot().intervalMs == 0);
    direct.record(true, 200, 3000, 65); // Valid duplicate ACK also confirms delivery.
    assert(direct.snapshot().intervalMs == 1000);
    direct.record(true, 201, 4200, 90);
    assert(direct.snapshot().intervalMs == 1050);
    assert(direct.snapshot().failures == 1);
    state("OK", 7699);
    state("LANGSAM", 7700);
    state("KEIN OK", 14200);
    state("WLAN FEHLT", 14200, false);
    assert(gateway.snapshot().acknowledgements == 0);
    gateway.record(false, 503, 5000, 500);
    assert(direct.snapshot().failures == 1);
    UploadStatus wrapped;
    const auto max = std::numeric_limits<uint32_t>::max();
    wrapped.record(true, 201, max - 499, 50);
    wrapped.record(true, 201, 500, 50);
    assert(wrapped.snapshot().intervalMs == 1000);
    assert(std::strcmp(uploadState(wrapped.snapshot(), true, true, 600), "OK") == 0);
    std::cout << "PASS: ACK validation state, cadence, stale/offline/error states, isolated paths, timer wrap\n";
}
