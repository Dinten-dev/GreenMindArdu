#pragma once
#include <string_view>

#ifndef GREENMIND_DIRECT_PROTOCOL_VERSION
#define GREENMIND_DIRECT_PROTOCOL_VERSION 1
#endif

#if defined(GREENMIND_DIRECT_DURABLE_SPOOL) && (defined(GREENMIND_DIRECT_OTA) || GREENMIND_DIRECT_PROTOCOL_VERSION != 1)
#error "Durable spool release requires its explicit USB-only protocol-1 target"
#endif

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
#if GREENMIND_DIRECT_PROTOCOL_VERSION == 2
#define GREENMIND_DIRECT_OTA_VERSION "direct-v2.15-rc1-raw400-live-p2-production"
#else
#define GREENMIND_DIRECT_OTA_VERSION "direct-v2.15-rc1-raw400-live-p1-production"
#endif
#endif
inline constexpr char firmwareVersion[] = GREENMIND_DIRECT_OTA_VERSION;
#else
#ifdef GREENMIND_DIRECT_DURABLE_SPOOL
inline constexpr char firmwareVersion[] = "direct-v2.16-rc3-dmaflash400-p1-production";
#elif GREENMIND_DIRECT_PROTOCOL_VERSION == 2
inline constexpr char firmwareVersion[] = "direct-v2.15-rc1-raw400-live-p2-production";
#else
inline constexpr char firmwareVersion[] = "direct-v2.15-rc1-raw400-live-p1-production";
#endif
#endif
#else
inline constexpr char host[] = "test.green-mind.ch";
inline constexpr char base[] = "https://test.green-mind.ch/api/v1/direct-ingest";
inline constexpr char endpoint[] = "https://test.green-mind.ch/api/v1/direct-ingest/chunks";
inline constexpr char label[] = "TESTUMGEBUNG";
// Preserve already paired Staging devices and their existing credentials.
inline constexpr char preferencesNamespace[] = "gmdirect";
#ifdef GREENMIND_DIRECT_DURABLE_SPOOL
inline constexpr char firmwareVersion[] = "direct-v2.16-rc3-dmaflash400-p1-staging";
#elif GREENMIND_DIRECT_PROTOCOL_VERSION == 2
inline constexpr char firmwareVersion[] = "direct-v2.15-rc4-raw400-live-p2-staging";
#else
inline constexpr char firmwareVersion[] = "direct-v2.15-rc4-raw400-live-p1-staging";
#endif
#endif
inline bool acceptsEndpoint(std::string_view candidate) {
    return candidate == endpoint;
}
// The native Direct ingest protocol accepts at most 50 firmware characters.
static_assert(sizeof(firmwareVersion) <= 51, "Direct firmware version exceeds cloud protocol limit");
static_assert(sizeof(preferencesNamespace) <= 16, "ESP32 NVS namespace limit");
}
