#pragma once

// Runs exclusively in directWorker. Reuses the existing raw HTTPS connection;
// never opens a second TLS connection or allocates a second upload task/stack.
static void directLiveTick(bool rawPending) {
    static uint32_t nextControl = 0, nextPacket = 0, sequence = 0;
    static char generation[37] = {};
    static bool wasActive = false;
    const uint32_t now = millis();
    const auto active = [] { return greenmind::liveDemand(millis(), liveUntil.load()); };
    if (!active()) wasActive = false;
    if (WiFi.status() != WL_CONNECTED || !hasClock.load() || !directTransport.connected()) return;
    const auto begin = [](const String& path) {
        if (!directHttp.begin(directTransport, String(greenmind::cloud::base) + "/devices/" + deviceId + path)) return false;
        directHttp.setReuse(true);
        directHttp.setConnectTimeout(100);
        // Arduino HTTPClient rounds the TLS socket timeout to whole seconds.
        // Below 500 ms that becomes zero and can truncate split responses.
        // Keep a bounded deadline with a nonzero underlying socket timeout.
        directHttp.setTimeout(750);
        directHttp.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
        directHttp.addHeader("Authorization", String("Bearer ") + token);
        return true;
    };
    if (!nextControl || static_cast<int32_t>(now - nextControl) >= 0) {
        nextControl = now + 1000;
        const uint32_t started = millis();
        const char* profile = mode == Mode::Direct ? greenmind::raw400PreviewProfile : greenmind::avg380PreviewProfile;
        String path = String("/control?session_id=") + sessionId + "&session_start_us=" +
            String(static_cast<unsigned long long>(sessionStartUs)) + "&source_rate=" + liveSourceRate +
            "&raw_rate=" + liveRawRate + "&preview_profile=" + profile +
            "&firmware_version=" + greenmind::cloud::firmwareVersion;
        if (begin(path)) {
            const int status = directHttp.GET();
            JsonDocument result;
            const int bytes = directHttp.getSize();
            String response;
            if (bytes >= 0 && bytes <= 2048) response = directHttp.getString();
            const bool parsed = bytes >= 0 && bytes <= 2048 && response.length() == static_cast<size_t>(bytes) &&
                !deserializeJson(result, response);
            if (status == 200 && parsed && result["enabled"] == true &&
                result["expires_in_ms"].is<uint32_t>() && result["expires_in_ms"].as<uint32_t>() <= 30000 &&
                result["generation"].is<const char*>() && strlen(result["generation"].as<const char*>()) == 36) {
                const char* incoming = result["generation"];
                portENTER_CRITICAL(&liveLock);
                const bool changed = strcmp(generation, incoming) != 0;
                if (!wasActive || changed) {
                    liveAverage.reset(liveSourceRate);
                    liveRails.reset(liveRawRate);
                    greenmind::LivePoint discarded;
                    while (livePoints.pop(discarded)) {}
                    memcpy(generation, incoming, 37);
                    if (changed) sequence = 0;
                }
                liveUntil.store(started + result["expires_in_ms"].as<uint32_t>());
                portEXIT_CRITICAL(&liveLock);
                wasActive = active();
            } else {
                liveUntil.store(0);
                wasActive = false;
                if (status == 404 || status == 405) nextControl = millis() + 30000;
            }
            const uint32_t elapsed = millis() - started;
            liveHttpMaximumMs.store(max(liveHttpMaximumMs.load(), elapsed));
            if (status != 200 || !parsed) liveHttpErrors.fetch_add(1);
            if (!parsed) directTransport.stop(); // Bounded body, never leave unread bytes for raw HTTP.
            directHttp.end();
        }
    }
    // Raw ACK/retry gets priority. Preview is lossy and never delays retained retries.
    if (rawPending || !active() || (nextPacket && static_cast<int32_t>(millis() - nextPacket) < 0)) return;
    nextPacket = millis() + 400;
    greenmind::LivePoint batch[8];
    uint8_t count = 0;
    greenmind::LivePoint point;
    const uint64_t cutoff = sessionStartUs + uint64_t(esp_timer_get_time() - liveOriginMonotonic);
    while (count < 8 && livePoints.pop(point)) {
        const uint64_t stamp = sessionStartUs + point.frame * 1000000ULL / liveSourceRate;
        if (stamp + 1000000 >= cutoff) batch[count++] = point;
    }
    if (!count) return;
    portENTER_CRITICAL(&liveLock);
    const auto rail = liveRails.snapshot();
    portEXIT_CRITICAL(&liveLock);
    if (!rail.endTick || rail.endTick <= rail.firstTick) return;
    JsonDocument body;
    body["generation"] = generation;
    body["session_id"] = sessionId;
    body["sequence"] = sequence++;
    JsonArray points = body["points"].to<JsonArray>();
    for (uint8_t i = 0; i < count; ++i) {
        JsonObject item = points.add<JsonObject>();
        item["frame"] = batch[i].frame;
        item["samples"] = batch[i].samples;
        item["mv"] = batch[i].mv;
    }
    body["rail"]["start_us"] = sessionStartUs + rail.firstTick * (1000000 / liveRawRate);
    body["rail"]["end_us"] = sessionStartUs + rail.endTick * (1000000 / liveRawRate);
    body["rail"]["raw_samples"] = rail.samples;
    body["rail"]["rail_samples"] = rail.railed;
    String payload;
    serializeJson(body, payload);
    if (payload.length() > 4096 || !begin("/live-preview")) return;
    directHttp.addHeader("Content-Type", "application/json");
    const uint32_t started = millis();
    const int status = directHttp.POST(payload);
    const int bytes = directHttp.getSize();
    if (bytes < 0 || bytes > 2048 || directHttp.getString().length() != static_cast<size_t>(bytes))
        directTransport.stop();
    directHttp.end();
    liveHttpMaximumMs.store(max(liveHttpMaximumMs.load(), uint32_t(millis() - started)));
    if (status == 200) livePackets.fetch_add(1);
    else liveHttpErrors.fetch_add(1);
}
