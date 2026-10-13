#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace greenmind {
inline constexpr char directOtaProfile[] = "biolingo-v22-n16r8-direct-ota-v1";
inline bool otaIdentifier(const char* value) {
    if (!value || std::strlen(value) < 1 || std::strlen(value) > 64) return false;
    for (const char* p = value; *p; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '.')) return false;
    return true;
}
inline bool otaHash(const char* value) {
    if (!value || std::strlen(value) != 64) return false;
    for (const char* p = value; *p; ++p)
        if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f'))) return false;
    return true;
}
inline bool otaOfferValid(const char* device, const char* expectedDevice,
                         const char* profile, const char* command,
                         const char* source, const char* current,
                         const char* target, const char* hash,
                         uint32_t bytes, uint32_t capacity) {
    return device && expectedDevice && std::strcmp(device, expectedDevice) == 0 &&
        profile && std::strcmp(profile, directOtaProfile) == 0 &&
        otaIdentifier(command) && otaIdentifier(source) && otaIdentifier(target) &&
        std::strcmp(source, current) == 0 && std::strcmp(target, current) != 0 &&
        otaHash(hash) && bytes >= 1024 && bytes <= capacity && bytes <= 0x300000;
}
inline bool otaImageHeader(const uint8_t* data, size_t bytes) {
    // Espressif image header: magic + ESP32-S3 chip ID (little endian).
    return bytes >= 24 && data[0] == 0xe9 && data[12] == 9 && data[13] == 0;
}
}
