#pragma once
#include <cstdint>
#include <climits>

namespace greenmind {
// Estimate the first filtered output's right edge in the monotonic sample grid.
// A formatted timestamp does not establish physical acquisition accuracy.
inline bool sampleClockStart(uint64_t syncEpochUs, uint64_t syncMonotonicUs,
                             uint64_t sampleMonotonicUs, uint64_t& startUs) {
    if (syncEpochUs < 1700000000000000ULL || sampleMonotonicUs < syncMonotonicUs)
        return false;
    const uint64_t elapsed = sampleMonotonicUs - syncMonotonicUs;
    if (elapsed > UINT64_MAX - syncEpochUs) return false;
    startUs = syncEpochUs + elapsed;
    return true;
}
}
