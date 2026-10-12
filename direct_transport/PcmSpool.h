#pragma once

#include "DirectCore.h"

namespace greenmind {
// Single acquisition producer, single upload consumer. Storage is allocated
// once, outside acquisition. A record remains owned until its exact ACK.
class PcmSpool {
    struct Header {
        uint64_t sequence;
        uint64_t firstFrame;
        uint16_t frames;
        uint16_t sampleRate;
        uint8_t channels;
        uint8_t sampleBits;
        uint16_t reserved;
    };
    static_assert(sizeof(Header) == 24, "Stable compact record size");
    uint8_t* storage_ = nullptr;
    uint32_t capacity_ = 0;
    std::atomic<uint32_t> head_{0}, tail_{0};

    void copyIn(uint32_t position, const void* source, size_t length) {
        const uint32_t offset = position & (capacity_ - 1);
        const size_t first = length < capacity_ - offset ? length : capacity_ - offset;
        std::memcpy(storage_ + offset, source, first);
        std::memcpy(storage_, static_cast<const uint8_t*>(source) + first, length - first);
    }
    void copyOut(uint32_t position, void* target, size_t length) const {
        const uint32_t offset = position & (capacity_ - 1);
        const size_t first = length < capacity_ - offset ? length : capacity_ - offset;
        std::memcpy(target, storage_ + offset, first);
        std::memcpy(static_cast<uint8_t*>(target) + first, storage_, length - first);
    }
public:
    // Power-of-two size preserves ring addressing across uint32 wraparound.
    bool begin(void* memory, uint32_t bytes) {
        if (storage_ || !memory || bytes < sizeof(Header) + SampleBlock::MaxBytes ||
            bytes > (1U << 30) || (bytes & (bytes - 1))) return false;
        storage_ = static_cast<uint8_t*>(memory);
        capacity_ = bytes;
        return true;
    }
    uint32_t capacity() const { return capacity_; }
    uint32_t used() const {
        const uint32_t tail = tail_.load(std::memory_order_acquire);
        const uint32_t value = head_.load(std::memory_order_acquire) - tail;
        return value > capacity_ ? capacity_ : value;
    }
    uint32_t capacitySeconds380() const { return capacity_ / (sizeof(Header) + 760); }
    bool push(const SampleBlock& block) {
        if (!storage_ || !block.valid()) return false;
        const uint32_t bytes = sizeof(Header) + block.byteCount();
        const uint32_t head = head_.load(std::memory_order_relaxed);
        const uint32_t used = head - tail_.load(std::memory_order_acquire);
        if (bytes > capacity_ - used) return false;
        const Header header{block.sequence, block.firstFrame, block.frames,
                            block.sampleRate, block.channels, block.sampleBits, 0};
        copyIn(head, &header, sizeof(header));
        copyIn(head + sizeof(header), block.payload, block.byteCount());
        head_.store(head + bytes, std::memory_order_release);
        return true;
    }
    bool peek(SampleBlock& block) const {
        if (!storage_) return false;
        const uint32_t tail = tail_.load(std::memory_order_relaxed);
        const uint32_t bytes = head_.load(std::memory_order_acquire) - tail;
        if (bytes < sizeof(Header)) return false;
        Header header{};
        copyOut(tail, &header, sizeof(header));
        block.sequence = header.sequence;
        block.firstFrame = header.firstFrame;
        block.frames = header.frames;
        block.sampleRate = header.sampleRate;
        block.channels = header.channels;
        block.sampleBits = header.sampleBits;
        if (!block.valid() || sizeof(Header) + block.byteCount() > bytes) return false;
        copyOut(tail + sizeof(Header), block.payload, block.byteCount());
        return true;
    }
    // Called only after sendDirect validates device/session/sequence/hash.
    bool acknowledge(uint64_t sequence) {
        if (!storage_) return false;
        const uint32_t tail = tail_.load(std::memory_order_relaxed);
        const uint32_t bytes = head_.load(std::memory_order_acquire) - tail;
        if (bytes < sizeof(Header)) return false;
        Header header{};
        copyOut(tail, &header, sizeof(header));
        const uint32_t length = sizeof(Header) +
            uint32_t(header.frames) * header.channels * (header.sampleBits / 8);
        if (header.sequence != sequence || length > bytes) return false;
        tail_.store(tail + length, std::memory_order_release);
        return true;
    }
};
}  // namespace greenmind
