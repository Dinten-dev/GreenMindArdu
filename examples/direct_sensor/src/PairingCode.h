#pragma once
#include <cstddef>

namespace greenmind {
// Call after trimming and uppercasing the portal input.
inline bool validPairingCode(const char* code, std::size_t length) {
    if (!code || (length != 6 && length != 8)) return false;
    for (std::size_t i = 0; i < length; ++i) {
        const char c = code[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) return false;
    }
    return true;
}
}
