#pragma once
#include <array>
#include <cstdint>
#include <cstring>

namespace greenmind {
inline constexpr uint8_t liveBatchPoints = 16;
inline constexpr uint8_t liveQueuePoints = 64;
inline constexpr char raw400PreviewProfile[] = "raw400-box32-v1";
inline constexpr char avg380PreviewProfile[] = "avg20ms380-rational-v1";

struct LivePoint {
    uint64_t frame = 0;
    double mv = 0;
    uint8_t samples = 0;
};

// Fractional bin clock: 400 -> exactly 32; 380 -> 30/31, never integer /30.
// Raw recording is never passed through this diagnostic-only accumulator.
class LiveAverager {
    uint16_t rate_ = 400, count_ = 0, phase_ = 0;
    uint64_t previous_ = 0;
    bool started_ = false;
    double sum_ = 0;
public:
    void reset(uint16_t rate) { rate_ = rate; count_ = phase_ = 0; sum_ = 0; started_ = false; }
    bool add(uint64_t frame, double mv, LivePoint& point) {
        if (rate_ != 400 && rate_ != 380) return false;
        if (started_ && frame != previous_ + 1) { count_ = phase_ = 0; sum_ = 0; }
        started_ = true; previous_ = frame;
        sum_ += mv; ++count_; phase_ += 25;
        if (phase_ < rate_ * 2) return false;
        phase_ -= rate_ * 2;
        point = {frame, sum_ / count_, static_cast<uint8_t>(count_)};
        sum_ = 0; count_ = 0;
        return true;
    }
};

struct RailSnapshot {
    uint64_t firstTick = 0, endTick = 0;
    uint32_t samples = 0, railed = 0;
};

// Two bits per nominal raw-ADC slot: valid and rail. At most 5000 bytes for
// 10 s * 2 kHz. Gaps are empty slots; missing values never become zero rails.
class LiveRailWindow {
    std::array<uint8_t, 5000> data_{};
    uint32_t size_ = 4000, samples_ = 0, railed_ = 0;
    uint64_t first_ = 0, last_ = 0;
    bool started_ = false;
    uint8_t get(uint32_t slot) const { return (data_[slot / 4] >> (slot % 4 * 2)) & 3; }
    void set(uint32_t slot, uint8_t value) {
        const uint8_t shift = slot % 4 * 2;
        data_[slot / 4] = (data_[slot / 4] & ~(3U << shift)) | (value << shift);
    }
    void erase(uint32_t slot) {
        const uint8_t old = get(slot);
        if (old & 1) --samples_;
        if (old & 2) --railed_;
        set(slot, 0);
    }
public:
    bool reset(uint16_t rawRate) {
        if (rawRate != 400 && rawRate != 2000) return false;
        size_ = rawRate * 10; samples_ = railed_ = 0; started_ = false;
        data_.fill(0);
        return true;
    }
    void add(uint64_t tick, int raw) {
        if (started_ && tick <= last_) return;
        if (!started_) { first_ = tick; started_ = true; }
        else if (tick - last_ >= size_) { data_.fill(0); samples_ = railed_ = 0; }
        else for (uint64_t i = last_ + 1; i <= tick; ++i) erase(i % size_);
        last_ = tick;
        if (raw < 0 || raw > 4095) return;
        uint8_t value = 1;
        ++samples_;
        if (raw == 0 || raw == 4095) { value |= 2; ++railed_; }
        set(tick % size_, value);
    }
    RailSnapshot snapshot() const {
        if (!started_) return {};
        const uint64_t lower = last_ + 1 >= size_ ? last_ + 1 - size_ : 0;
        return {first_ > lower ? first_ : lower, last_ + 1, samples_, railed_};
    }
};

// Wrap-safe 30-second demand clock, usable without network/task scheduling.
inline bool liveDemand(uint32_t now, uint32_t until) {
    return until != 0 && static_cast<int32_t>(until - now) > 0;
}
}  // namespace greenmind
