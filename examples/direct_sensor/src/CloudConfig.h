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
inline constexpr char firmwareVersion[] = "direct-hotspot-v2.7-production";
#else
inline constexpr char host[] = "test.green-mind.ch";
inline constexpr char base[] = "https://test.green-mind.ch/api/v1/direct-ingest";
inline constexpr char endpoint[] = "https://test.green-mind.ch/api/v1/direct-ingest/chunks";
inline constexpr char label[] = "TESTUMGEBUNG";
// Preserve already paired Staging devices and their existing credentials.
inline constexpr char preferencesNamespace[] = "gmdirect";
inline constexpr char firmwareVersion[] = "direct-hotspot-v2.7-staging";
#endif
inline bool acceptsEndpoint(std::string_view candidate) {
    return candidate == endpoint;
}
static_assert(sizeof(preferencesNamespace) <= 16, "ESP32 NVS namespace limit");
}
