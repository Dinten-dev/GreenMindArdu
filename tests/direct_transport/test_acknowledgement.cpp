#include "../../transport_validation/Acknowledgement.h"
#include <cassert>
#include <iostream>

int main() {
    JsonDocument ack;
    ack["status"] = "queued";
    ack["boot_id"] = 123;
    ack["sequence"] = 0;
    ack["samples_archived"] = 380;
    assert(greenmind::gatewayAcknowledged(ack.as<JsonVariantConst>(), 123, 0, 380));
    assert(!greenmind::gatewayAcknowledged(ack.as<JsonVariantConst>(), 124, 0, 380));
    assert(!greenmind::gatewayAcknowledged(ack.as<JsonVariantConst>(), 123, 0, 379));
    ack.remove("sequence");
    assert(!greenmind::gatewayAcknowledged(ack.as<JsonVariantConst>(), 123, 0, 380));
    ack["sequence"] = "0";
    assert(!greenmind::gatewayAcknowledged(ack.as<JsonVariantConst>(), 123, 0, 380));
    ack["sequence"] = 0;
    ack["status"] = "error";
    assert(!greenmind::gatewayAcknowledged(ack.as<JsonVariantConst>(), 123, 0, 380));
    ack["status"] = "duplicate";
    assert(greenmind::gatewayAcknowledged(ack.as<JsonVariantConst>(), 123, 0, 380));
    ack.remove("boot_id");
    assert(!greenmind::gatewayAcknowledged(ack.as<JsonVariantConst>(), 0, 0, 380));

    ack.clear();
    ack["status"] = "persisted";
    ack["device_id"] = "device";
    ack["session_id"] = "session";
    ack["payload_sha256"] = "digest";
    assert(!greenmind::directAcknowledged(ack.as<JsonVariantConst>(), "device", "session", 0, "digest"));
    for (const char* malformed : {"\"0\"", "null", "false", "-1", "0.5"}) {
        JsonDocument value;
        assert(!deserializeJson(value, malformed));
        ack["sequence"] = value.as<JsonVariantConst>();
        assert(!greenmind::directAcknowledged(ack.as<JsonVariantConst>(), "device", "session", 0, "digest"));
    }
    ack["sequence"] = uint64_t(4294967296ULL);
    assert(greenmind::directAcknowledged(ack.as<JsonVariantConst>(), "device", "session", 4294967296ULL, "digest"));
    assert(!greenmind::directAcknowledged(ack.as<JsonVariantConst>(), "device", "other", 4294967296ULL, "digest"));
    assert(!greenmind::directAcknowledged(ack.as<JsonVariantConst>(), "other", "session", 4294967296ULL, "digest"));
    assert(!greenmind::directAcknowledged(ack.as<JsonVariantConst>(), "device", "session", 4294967296ULL, "wrong"));

    // Regression: an idle deadline used to halt uploads after ~24.9 days.
    for (uint32_t now : {0U, 100U, 0x7fffffffU, 0x80000000U, 0xffffffffU})
        assert(!greenmind::retryPending(now, 0));
    assert(greenmind::retryPending(100, 200));
    assert(!greenmind::retryPending(200, 200));
    assert(!greenmind::retryPending(201, 200));
    assert(greenmind::retryPending(0xfffffff0U, 20));
    assert(!greenmind::retryPending(21, 20));
    std::cout << "PASS: complete typed ACKs and millis rollover\n";
}
