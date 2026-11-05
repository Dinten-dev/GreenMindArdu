#pragma once
#include "../../../direct_transport/DirectLiveBudget.h"

// One TLS owner. A raw block remains immutable until its exact acknowledgement.
// Live can use retry cooldown time; it never replaces the pending raw block.
static void directWorkerStep() {
    static bool retained = false, retryScheduled = false, failed = false;
    static uint32_t nextRetry = 0, backoff = 1000;
    if (failed) return;
    const bool interactive = mode == Mode::Direct &&
        greenmind::liveDemand(millis(), liveUntil.load());
    // Sleeping between DTIM beacons adds receive latency on phone hotspots.
    // Restore the normal radio power policy as soon as viewer demand expires.
    static bool radioAwake = false;
    if (interactive != radioAwake && WiFi.setSleep(!interactive)) {
        radioAwake = interactive;
        Serial.printf("direct_live_radio awake=%d\n", radioAwake);
    }
    if (!retained) {
        directOtaTick(); // OTA may drain the spool; never run during retained retries.
        if (!directQueue.peek(directPending)) { directLiveTick(false); return; }
        retained = true;
    }
    if (retryScheduled && static_cast<int32_t>(millis() - nextRetry) < 0) {
        if (interactive) directLiveTick(false, true);
        return;
    }
    if (!sendDirect(directPending)) {
        nextRetry = millis() + greenmind::rawRetryWait(interactive, backoff, esp_random());
        backoff = interactive ? 1000 : min(uint32_t(60000), backoff * 2);
        retryScheduled = true;
        return;
    }
    if (!directQueue.acknowledge(directPending.sequence)) {
        Serial.printf("%s\n", "direct_spool_ack_invariant_failed");
        failed = true; return;
    }
    retained = retryScheduled = false;
    backoff = 1000;
    const auto queued = directQueue.used();
    const bool allowPreview = greenmind::previewAfterRawAck(interactive, queued);
    directLiveTick(queued != 0 && !allowPreview);
}
