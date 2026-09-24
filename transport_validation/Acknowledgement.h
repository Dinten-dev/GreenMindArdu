#pragma once
#include <ArduinoJson.h>
#include <stdint.h>
#include <string.h>

namespace greenmind {
// Numeric conversion alone treats a missing sequence as zero. Never erase a
// pending packet unless its complete, typed identity has been acknowledged.
inline bool gatewayAcknowledged(JsonVariantConst ack, uint32_t bootId, uint32_t sequence,
                                uint32_t samples) {
    return ack["boot_id"].is<uint32_t>() && ack["sequence"].is<uint32_t>() &&
           ack["samples_archived"].is<uint32_t>() && ack["boot_id"].as<uint32_t>() == bootId &&
           ack["sequence"].as<uint32_t>() == sequence &&
           ack["samples_archived"].as<uint32_t>() == samples &&
           (ack["status"] == "queued" || ack["status"] == "duplicate");
}

inline bool directAcknowledged(JsonVariantConst ack, const char* deviceId, const char* sessionId,
                               uint64_t sequence, const char* digest) {
    return ack["sequence"].is<uint64_t>() && ack["sequence"].as<uint64_t>() == sequence &&
           ack["device_id"].is<const char*>() && ack["device_id"] == deviceId &&
           ack["session_id"].is<const char*>() && ack["session_id"] == sessionId &&
           ack["payload_sha256"].is<const char*>() && ack["payload_sha256"] == digest &&
           (ack["status"] == "persisted" || ack["status"] == "duplicate");
}

inline bool retryPending(uint32_t now, uint32_t deadline) {
    // Zero means no backoff, including after the signed millis() half-period.
    return deadline != 0 && static_cast<int32_t>(now - deadline) < 0;
}
} // namespace greenmind
