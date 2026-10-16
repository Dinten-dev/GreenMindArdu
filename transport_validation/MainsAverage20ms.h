#pragma once
#include <array>
#include <cstdint>

namespace greenmind {
// Forty real, uniformly hardware-clocked ADC conversions. No invented catch-up.
// Nominal zeros at 50 Hz and its harmonics; ADC clipping is not recoverable.
class MainsAverage20ms {
public:
    static constexpr uint32_t rawHz = 2000;
    static constexpr uint32_t outputHz = 380;
    static constexpr uint32_t samples = rawHz / 50;
    static constexpr uint32_t groupDelayUs = (samples - 1) * 1000000 / rawHz / 2;
    void reset() { sum_ = 0; count_ = 0; position_ = 0; }
    bool add(uint16_t raw) {
        if (count_ == samples) sum_ -= ring_[position_];
        else ++count_;
        ring_[position_] = raw;
        sum_ += raw;
        position_ = (position_ + 1) % samples;
        return count_ == samples;
    }
    double mean() const { return count_ ? double(sum_) / count_ : 0; }
private:
    std::array<uint16_t, samples> ring_{};
    uint32_t sum_ = 0, count_ = 0, position_ = 0;
};
}
