#include "../../direct_transport/BootIdentity.h"
#include "../../direct_transport/CaptureClock.h"
#include <cassert>
#include <cstring>
#include <iostream>

struct Store {
    greenmind::BootIdentity saved{};
    bool exists = false, readError = false, writeError = false, corruptReadback = false;
    greenmind::IdentityRead read(greenmind::BootIdentity& out) {
        if (readError) return greenmind::IdentityRead::Error;
        if (!exists) return greenmind::IdentityRead::Missing;
        out = saved;
        if (corruptReadback) ++out.counter;
        return greenmind::IdentityRead::Valid;
    }
    bool writeAndCommit(const greenmind::BootIdentity& in) {
        if (writeError) return false;
        saved = in; exists = true; return true;
    }
};
int main() {
    uint64_t start = 0;
    assert(!greenmind::sampleClockStart(0, 0, 10000, start));
    assert(!greenmind::sampleClockStart(1700000000000000ULL, 100, 99, start));
    assert(greenmind::sampleClockStart(1700000000000000ULL, 100, 10100, start));
    assert(start == 1700000000010000ULL);
    Store s;
    unsigned seed = 1;
    auto random = [&seed](uint8_t* data, size_t n) { std::memset(data, seed++, n); };
    greenmind::BootIdentity first{}, next{};
    assert(greenmind::allocateBootIdentity(s, random, first));
    assert(first.counter == 1);
    assert(greenmind::allocateBootIdentity(s, random, next));
    assert(next.counter == 2 && std::memcmp(first.generation, next.generation, 16) == 0);
    s.writeError = true;
    assert(!greenmind::allocateBootIdentity(s, random, next));
    s.writeError = false;
    s.readError = true;
    assert(!greenmind::allocateBootIdentity(s, random, next));
    s.readError = false;
    s.corruptReadback = true;
    assert(!greenmind::allocateBootIdentity(s, random, next));
    s.corruptReadback = false;
    s.exists = false; // Factory-reset generation changes.
    assert(greenmind::allocateBootIdentity(s, random, next));
    assert(next.counter == 1 && std::memcmp(first.generation, next.generation, 16) != 0);
    s.saved.counter = INT64_MAX;
    assert(!greenmind::allocateBootIdentity(s, random, next));
    std::cout << "PASS: boot increment, factory reset, read/write/verification failure, overflow\n";
}
