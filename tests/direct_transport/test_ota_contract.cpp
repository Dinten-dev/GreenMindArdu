#include "../../direct_transport/DirectOtaContract.h"
#include <cassert>
#include <iostream>
#include <string>

int main() {
    using namespace greenmind;
    const std::string sha(64, 'a');
    auto valid = [&](const char* device, const char* profile, const char* command,
                     const char* from, const char* target, const char* hash,
                     uint32_t bytes, uint32_t capacity = 0x300000) {
        return otaOfferValid(device, "sensor", profile, command, from, "seed", target,
                             hash, bytes, capacity);
    };
    assert(valid("sensor", directOtaProfile, "cmd-1", "seed", "trial", sha.c_str(), 1024));
    assert(!valid("other", directOtaProfile, "cmd-1", "seed", "trial", sha.c_str(), 1024));
    assert(!valid("sensor", "other-board", "cmd-1", "seed", "trial", sha.c_str(), 1024));
    assert(!valid("sensor", directOtaProfile, "../escape", "seed", "trial", sha.c_str(), 1024));
    assert(!valid("sensor", directOtaProfile, "cmd", "other", "trial", sha.c_str(), 1024));
    assert(!valid("sensor", directOtaProfile, "cmd", "seed", "seed", sha.c_str(), 1024));
    assert(!valid("sensor", directOtaProfile, "cmd", "seed", "trial", "abcdef", 1024));
    const std::string badHash(64, 'g');
    assert(!valid("sensor", directOtaProfile, "cmd", "seed", "trial", badHash.c_str(), 1024));
    assert(!valid("sensor", directOtaProfile, "cmd", "seed", "trial", sha.c_str(), 0));
    assert(!valid("sensor", directOtaProfile, "cmd", "seed", "trial", sha.c_str(), 0x300001));
    assert(!valid("sensor", directOtaProfile, "cmd", "seed", "trial", sha.c_str(), 2048, 1024));
    uint8_t image[24] = {};
    image[0] = 0xe9; image[12] = 9;
    assert(otaImageHeader(image, sizeof(image)));
    assert(!otaImageHeader(image, 23));
    image[12] = 0;
    assert(!otaImageHeader(image, sizeof(image)));
    std::cout << "PASS OTA device/profile/version/path/hash/size/chip guards\n";
}
