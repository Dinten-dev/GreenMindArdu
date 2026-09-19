#pragma once

#include <atomic>
#include <cstdint>

namespace greenmind {
struct UploadSnapshot {
    uint32_t acknowledgements, failures, lastAckMs, intervalMs, durationMs;
    int result;
    bool lastSucceeded;
};

// One upload task owns each instance. The display only reads atomic counters.
struct UploadStatus {
    std::atomic<uint32_t> acknowledgements{0}, failures{0}, lastAckMs{0};
    std::atomic<uint32_t> intervalMs{0}, durationMs{0};
    std::atomic<int> result{0};
    std::atomic<bool> lastSucceeded{false};

    void record(bool ok, int code, uint32_t now, uint32_t duration) {
        result.store(code);
        durationMs.store(duration);
        if (ok) {
            const uint32_t count = acknowledgements.load();
            if (count) {
                const uint32_t elapsed = now - lastAckMs.load();
                // Smooth the measured confirmation interval, not a fixed timer.
                const uint32_t previous = intervalMs.load();
                intervalMs.store(count == 1 ? elapsed :
                    static_cast<uint32_t>((uint64_t(previous) * 3 + elapsed) / 4));
            }
            lastAckMs.store(now);
            acknowledgements.fetch_add(1);
        } else {
            failures.fetch_add(1);
        }
        lastSucceeded.store(ok);
    }
    UploadSnapshot snapshot() const {
        return {acknowledgements.load(), failures.load(), lastAckMs.load(),
                intervalMs.load(), durationMs.load(), result.load(), lastSucceeded.load()};
    }
};

inline const char* uploadState(const UploadSnapshot& s, bool wifi, bool clockReady,
                               uint32_t now) {
    if (!wifi) return "WLAN FEHLT";
    if (!clockReady) return "WARTE UHR";
    if (!s.lastSucceeded && s.failures) return "SENDEFEHLER";
    if (!s.acknowledgements) return "WARTE OK";
    const uint32_t age = now - s.lastAckMs;
    if (age >= 10000) return "KEIN OK";
    if (age >= 3500) return "LANGSAM";
    return "OK";
}
} // namespace greenmind
