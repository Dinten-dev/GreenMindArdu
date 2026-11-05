#pragma once
#include <cstdint>

namespace greenmind {
struct DirectNetworkBudget {
    uint32_t readMs, connectMs, handshakeSeconds;
};
inline constexpr DirectNetworkBudget directNetworkBudget(bool interactive) {
    return interactive ? DirectNetworkBudget{750, 250, 1}
                       : DirectNetworkBudget{10000, 5000, 10};
}
inline constexpr bool previewAfterRawAck(bool interactive, uint32_t queuedBytes) {
    return interactive && queuedBytes <= 8192;
}
inline constexpr uint32_t rawRetryWait(bool interactive, uint32_t backoff, uint32_t random) {
    return interactive ? 500 + random % 100 : backoff + random % 500;
}
}  // namespace greenmind
