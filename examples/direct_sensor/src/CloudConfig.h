#pragma once
#include <string_view>

namespace greenmind::cloud {
#ifdef GREENMIND_CLOUD_PRODUCTION
inline constexpr char host[] = "green-mind.ch";
inline constexpr char base[] = "https://green-mind.ch/api/v1/direct-ingest";
inline constexpr char endpoint[] = "https://green-mind.ch/api/v1/direct-ingest/chunks";
inline constexpr char label[] = "PRODUCTION";
// Never reuse a Staging device identity/token when changing environments.
inline constexpr char preferencesNamespace[] = "gmdirectprod";
#ifdef GREENMIND_DIRECT_OTA
#ifndef GREENMIND_DIRECT_OTA_VERSION
#define GREENMIND_DIRECT_OTA_VERSION "direct-hotspot-v2.11-rc1-avg20ms-production"
#endif
inline constexpr char firmwareVersion[] = GREENMIND_DIRECT_OTA_VERSION;
#else
inline constexpr char firmwareVersion[] = "direct-hotspot-v2.11-rc1-avg20ms-production";
#endif
#else
inline constexpr char host[] = "test.green-mind.ch";
inline constexpr char base[] = "https://test.green-mind.ch/api/v1/direct-ingest";
inline constexpr char endpoint[] = "https://test.green-mind.ch/api/v1/direct-ingest/chunks";
inline constexpr char label[] = "TESTUMGEBUNG";
// Preserve already paired Staging devices and their existing credentials.
inline constexpr char preferencesNamespace[] = "gmdirect";
inline constexpr char firmwareVersion[] = "direct-hotspot-v2.11-rc1-avg20ms-staging";
#endif
inline bool acceptsEndpoint(std::string_view candidate) {
    return candidate == endpoint;
}
static_assert(sizeof(preferencesNamespace) <= 16, "ESP32 NVS namespace limit");
}
