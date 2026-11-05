#pragma once

// Only directWorker owns HTTP/TLS. Acquisition remains independent and raw
// backlog always wins. A bounded pending body survives a lost preview ACK.
static void directLiveTick(bool rawPending, bool reconnectAllowed = false) {
    static uint32_t nextControl = 0, nextPacket = 0, sequence = 0, pendingSince = 0;
    static char generation[37] = {};
    static bool wasActive = false;
    static String pendingPayload;
    static uint8_t pendingCount = 0;
    const auto clearPending = [] { pendingPayload = ""; pendingCount = 0; };
    const auto active = [] { return greenmind::liveDemand(millis(), liveUntil.load()); };
    if (!active()) {
        if (wasActive) nextControl = 0;
        wasActive = false; clearPending();
    }
    if (rawPending || WiFi.status() != WL_CONNECTED || !hasClock.load() ||
        (!directTransport.connected() && !reconnectAllowed)) return;
    const auto begin = [](const String& path) {
        directTransport.setHandshakeTimeout(1);
        if (!directHttp.begin(directTransport, String(greenmind::cloud::base) + "/devices/" + deviceId + path)) return false;
        directHttp.setReuse(true);
        directHttp.setConnectTimeout(100);
        // HTTPClient rounds the TLS socket timeout to whole seconds.
        directHttp.setTimeout(750);
        directHttp.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
        directHttp.addHeader("Authorization", String("Bearer ") + token);
        return true;
    };
    const auto demand = [&](const JsonDocument& result, uint32_t requested) {
        if (result["enabled"] != true || !result["expires_in_ms"].is<uint32_t>() ||
            result["expires_in_ms"].as<uint32_t>() > 30000 ||
            !result["generation"].is<const char*>() || strlen(result["generation"].as<const char*>()) != 36)
            return false;
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
            clearPending();
        }
        // Count from the request, never extend the server's actual viewer lease.
        liveUntil.store(requested + result["expires_in_ms"].as<uint32_t>());
        portEXIT_CRITICAL(&liveLock);
        wasActive = active();
        return true;
    };
    const uint32_t now = millis();
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
            if (status == 200 && parsed) {
                if (!demand(result, started)) {
                    liveUntil.store(0); wasActive = false; clearPending();
                }
            } else {
                // A transient timeout cannot revoke already confirmed demand.
                if (status == 401 || status == 403 || status == 404 || status == 405) {
                    liveUntil.store(0); wasActive = false; clearPending();
                }
                if (status == 404 || status == 405) nextControl = millis() + 30000;
            }
            liveHttpMaximumMs.store(max(liveHttpMaximumMs.load(), uint32_t(millis() - started)));
            if (status != 200 || !parsed) liveHttpErrors.fetch_add(1);
            if (!parsed) directTransport.stop();
            directHttp.end();
        }
    }
    if (!active() || (nextPacket && static_cast<int32_t>(millis() - nextPacket) < 0)) return;
    if (pendingCount && uint32_t(millis() - pendingSince) > 1500) {
        liveDropped.fetch_add(pendingCount); clearPending();
    }
    if (!pendingCount) {
        greenmind::LivePoint batch[greenmind::liveBatchPoints];
        uint8_t count = 0;
        greenmind::LivePoint point;
        const uint64_t cutoff = sessionStartUs + uint64_t(esp_timer_get_time() - liveOriginMonotonic);
        while (count < greenmind::liveBatchPoints && livePoints.pop(point)) {
            const uint64_t stamp = sessionStartUs + point.frame * 1000000ULL / liveSourceRate;
            const uint64_t retention = liveSourceRate == 400 ? 3000000 : 2000000;
            if (stamp + retention >= cutoff) batch[count++] = point;
            else liveDropped.fetch_add(1);
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
        pendingPayload = "";
        serializeJson(body, pendingPayload);
        if (pendingPayload.length() > 4096) { liveDropped.fetch_add(count); pendingPayload = ""; return; }
        pendingCount = count;
        pendingSince = millis();
    }
    if (!begin("/live-preview")) return;
    directHttp.addHeader("Content-Type", "application/json");
    const uint32_t started = millis();
    const int status = directHttp.POST(pendingPayload);
    const int bytes = directHttp.getSize();
    String response;
    if (bytes >= 0 && bytes <= 2048) response = directHttp.getString();
    JsonDocument ack;
    const bool parsed = bytes >= 0 && bytes <= 2048 && response.length() == static_cast<size_t>(bytes) &&
        !deserializeJson(ack, response);
    const bool accepted = status == 200 && parsed &&
        (ack["status"] == "accepted" || ack["status"] == "duplicate");
    if (accepted) {
        // Clear the acknowledged immutable body before refreshing demand, which
        // may clear it again when the server changes the generation.
        clearPending();
        livePackets.fetch_add(1);
        if (ack["generation"] == generation && demand(ack, started) && active())
            nextControl = millis() + 10000;
        else nextControl = millis() + 1000;
        nextPacket = started + 400;
    } else {
        liveHttpErrors.fetch_add(1);
        nextPacket = millis() + 200;
        if (status == 400 || status == 401 || status == 403 || status == 404 || status == 409 || status == 422) {
            liveDropped.fetch_add(pendingCount); clearPending();
            liveUntil.store(0); wasActive = false; nextControl = 0;
        }
    }
    if (!parsed) directTransport.stop();
    directHttp.end();
    liveHttpMaximumMs.store(max(liveHttpMaximumMs.load(), uint32_t(millis() - started)));
}
