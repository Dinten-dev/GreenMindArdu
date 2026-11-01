#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <climits>

namespace greenmind {
struct BootIdentity { uint32_t magic; uint8_t generation[16]; uint64_t counter; };
enum class IdentityRead { Missing, Valid, Error };

// Store implementation must atomically commit one blob. Any error fails closed.
template<class Store, class Random>
bool allocateBootIdentity(Store& store, Random random, BootIdentity& result) {
    constexpr uint32_t magic = 0x474d4232;
    BootIdentity value{};
    const IdentityRead status = store.read(value);
    if (status == IdentityRead::Missing) {
        value.magic = magic;
        random(value.generation, sizeof(value.generation));
        value.generation[6] = (value.generation[6] & 0x0f) | 0x40;
        value.generation[8] = (value.generation[8] & 0x3f) | 0x80;
    } else if (status != IdentityRead::Valid || value.magic != magic ||
               value.counter == 0 || value.counter >= INT64_MAX) return false;
    ++value.counter;
    if (!store.writeAndCommit(value)) return false;
    BootIdentity verify{};
    if (store.read(verify) != IdentityRead::Valid ||
        std::memcmp(&value, &verify, sizeof(value)) != 0) return false;
    result = value;
    return true;
}
}  // namespace greenmind
