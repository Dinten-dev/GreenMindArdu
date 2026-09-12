#pragma once

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace greenmind {
enum class Mode { Gateway, Direct, Dual, Invalid };

inline Mode parseMode(const char* value) {
    if (!value || !*value || std::strcmp(value, "GATEWAY") == 0) return Mode::Gateway;
    if (std::strcmp(value, "DIRECT") == 0) return Mode::Direct;
    if (std::strcmp(value, "DUAL") == 0) return Mode::Dual;
    return Mode::Invalid;
}

inline int16_t legacyPcm(double millivolts) {
    const double clipped = millivolts < 0 ? 0 : (millivolts > 3300 ? 3300 : millivolts);
    return static_cast<int16_t>(clipped * (32767.0 / 3300.0));
}

inline bool pack24(int32_t value, uint8_t* output) {
    if (value < -8388608 || value > 8388607) return false;
    const uint32_t bits = static_cast<uint32_t>(value);
    output[0] = static_cast<uint8_t>(bits);
    output[1] = static_cast<uint8_t>(bits >> 8);
    output[2] = static_cast<uint8_t>(bits >> 16);
    return true;
}

struct SampleBlock {
    static constexpr size_t MaxBytes = 6000;
    uint64_t sequence = 0;
    uint64_t firstFrame = 0;
    uint16_t frames = 0;
    uint16_t sampleRate = 380;
    uint8_t channels = 1;
    uint8_t sampleBits = 16;
    uint8_t payload[MaxBytes] = {};
    // Optional comparison values: present only for mono AD8232 Gateway/DUAL.
    // Fixed tenths of a millivolt avoid independent float-rounding paths.
    uint16_t gatewayDeciMv[500] = {};

    size_t byteCount() const { return size_t(frames) * channels * (sampleBits / 8); }
    bool valid() const {
        return frames > 0 && sampleRate > 0 && sampleRate <= 2000 && channels > 0 && channels <= 8 &&
               (sampleBits == 16 || sampleBits == 24) && frames <= sampleRate * 10 && byteCount() <= MaxBytes;
    }
};

// One acquisition producer and one network consumer per queue. Each slot owns
// its data; no pointer to a rotating acquisition buffer can reach the network.
// This replaceable boundary can later be backed by a durable SD-card spool.
template<typename T, size_t Capacity>
class SpscQueue {
    static_assert(Capacity > 0, "Queue must be bounded and non-empty");
    T slots_[Capacity + 1];
    std::atomic<size_t> head_{0};
    std::atomic<size_t> tail_{0};
public:
    bool push(const T& value) {
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t next = (head + 1) % (Capacity + 1);
        if (next == tail_.load(std::memory_order_acquire)) return false;
        slots_[head] = value;
        head_.store(next, std::memory_order_release);
        return true;
    }
    bool pop(T& value) {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) return false;
        value = slots_[tail];
        tail_.store((tail + 1) % (Capacity + 1), std::memory_order_release);
        return true;
    }
};
}  // namespace greenmind
