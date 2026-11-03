#pragma once
#include "DirectCore.h"

namespace greenmind {
inline constexpr uint32_t raw400Hz = 400;
inline constexpr uint64_t raw400IntervalUs = 1000000 / raw400Hz;
inline constexpr char raw400Profile[] = "ad8232-raw400-v1";
static_assert(1000000 % raw400Hz == 0, "Exact nominal sampling interval required");

// Same nominal 0..3300 mV -> 0..32767 PCM scale as existing GreenMind files.
// No mV*10 intermediate: 3300*10 would overflow signed PCM16. All 4096 ADC
// codes remain distinct, including both analogue rails. No digital clipping.
inline bool encodeRaw400(uint16_t raw, int16_t& pcm, uint16_t& deciMv) {
    if (raw > 4095) return false;
    pcm = legacyPcm(double(raw) * 3300.0 / 4095.0);
    deciMv = static_cast<uint16_t>(std::lround(double(raw) * 33000.0 / 4095.0));
    return true;
}

// Use actual callback timestamps to reject catch-up bursts and expose missed
// slots. Never manufacture conversions to fill a nominal time grid. Phase is
// anchored to the first real reading; accuracy remains a hardware measurement.
class Raw400Clock {
    uint64_t origin_ = 0, previous_ = 0;
    bool started_ = false;
public:
    uint64_t originUs() const { return origin_; }
    bool admit(uint64_t now, uint64_t& frame, uint64_t& missing, uint32_t& jitter) {
        missing = 0;
        jitter = 0;
        if (!started_) {
            started_ = true;
            origin_ = now;
            previous_ = frame = 0;
            return true;
        }
        if (now < origin_) return false;
        const uint64_t elapsed = now - origin_;
        frame = (elapsed + raw400IntervalUs / 2) / raw400IntervalUs;
        if (frame <= previous_) return false;
        missing = frame - previous_ - 1;
        const uint64_t nominal = frame * raw400IntervalUs;
        jitter = static_cast<uint32_t>(elapsed > nominal ? elapsed - nominal : nominal - elapsed);
        previous_ = frame;
        return true;
    }
};
}  // namespace greenmind
