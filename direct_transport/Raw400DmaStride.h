#pragma once
#include <cstdint>
namespace greenmind {
// One untouched ADC value out of each five 2 kHz hardware conversions.
// No averaging, interpolation, repeated values or digital filtering.
class Raw400DmaStride {
    uint64_t conversions_ = 0;
public:
    static constexpr uint32_t inputHz = 2000, stride = 5, outputHz = 400;
    bool take(uint64_t& frame) {
        const uint64_t index = conversions_++;
        frame = index / stride;
        return index % stride == 0;
    }
    uint64_t conversions() const { return conversions_; }
};
class Raw400DmaDeliveryGuard {
    uint64_t previous_;
public:
    static constexpr uint32_t maximumGapUs = 400000;
    explicit Raw400DmaDeliveryGuard(uint64_t startedAt) : previous_(startedAt) {}
    bool observe(uint64_t now) {
        if (now < previous_ || now - previous_ > maximumGapUs) return false;
        previous_ = now; return true;
    }
};
}
