#pragma once
#include "../../../direct_transport/DirectLiveBudget.h"

// One TLS owner. A raw block remains immutable until its exact acknowledgement.
// Live can use retry cooldown time; it never replaces the pending raw block.
static void directWorkerStep() {
    static bool retained = false, retryScheduled = false, failed = false;
    static uint32_t nextRetry = 0, backoff = 1000;
#ifdef GREENMIND_DIRECT_DURABLE_SPOOL
    static std::string frozenMetadata;
#endif
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
#ifdef GREENMIND_DIRECT_DURABLE_SPOOL
        if (!directFlashPeek(directPending, frozenMetadata)) { directLiveTick(false); return; }
#else
        if (!directQueue.peek(directPending)) { directLiveTick(false); return; }
#endif
        retained = true;
    }
    if (retryScheduled && static_cast<int32_t>(millis() - nextRetry) < 0) {
        if (interactive) directLiveTick(false, true);
        return;
    }
#ifdef GREENMIND_DIRECT_DURABLE_SPOOL
    const bool sent = sendDirect(directPending, &frozenMetadata);
#else
    const bool sent = sendDirect(directPending);
#endif
    if (!sent) {
        nextRetry = millis() + greenmind::rawRetryWait(interactive, backoff, esp_random());
        backoff = interactive ? 1000 : min(uint32_t(60000), backoff * 2);
        retryScheduled = true;
        return;
    }
#ifdef GREENMIND_DIRECT_DURABLE_SPOOL
    const bool acknowledged = directFlashAcknowledge(directPending.sequence);
#else
    const bool acknowledged = directQueue.acknowledge(directPending.sequence);
#endif
    if (!acknowledged) {
        Serial.printf("%s\n", "direct_spool_ack_invariant_failed");
        failed = true; return;
    }
    retained = retryScheduled = false;
    backoff = 1000;
#ifdef GREENMIND_DIRECT_DURABLE_SPOOL
    const auto queued = directFlashPendingBytes.load();
#else
    const auto queued = directQueue.used();
#endif
    const bool allowPreview = greenmind::previewAfterRawAck(interactive, queued);
    directLiveTick(queued != 0 && !allowPreview);
}
