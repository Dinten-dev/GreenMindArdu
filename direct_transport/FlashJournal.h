#pragma once

#include "DirectCore.h"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace greenmind {
// LittleFS/VFS on ESP32; the same record/recovery code runs against native files.
// Single writer and single reader must serialize access outside this class.
class FlashJournal {
#pragma pack(push, 1)
    struct FileHeader {
        uint32_t magic;
        uint16_t version, metadataBytes, sampleRate;
        uint8_t channels, sampleBits;
        uint32_t crc;
    };
    struct RecordHeader {
        uint32_t magic;
        uint16_t bytes, frames;
        uint64_t sequence, firstFrame;
        uint32_t crc;
    };
#pragma pack(pop)
    static_assert(sizeof(FileHeader) == 16 && sizeof(RecordHeader) == 28, "Journal format changed");
    static constexpr uint32_t FileMagic = 0x31464d47, RecordMagic = 0x31524d47;
    static constexpr size_t SegmentBytes = 64 * 1024, ReserveBytes = 64 * 1024;
    std::string directory_, writing_, reading_, writingMetadata_, pendingMetadata_;
    uint64_t next_ = 0;
    size_t maximum_ = 0, used_ = 0, queuedBytes_ = 0, offset_ = 0, nextOffset_ = 0;
    uint64_t pendingSequence_ = 0;
    bool pending_ = false, failed_ = false;
    uint32_t incompleteRecovered_ = 0;

    static uint32_t crc(const void* data, size_t length, uint32_t value = 0xffffffffU) {
        auto p = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < length; ++i) {
            value ^= p[i];
            for (unsigned b = 0; b < 8; ++b) value = (value >> 1) ^ (0xedb88320U & (0U - (value & 1U)));
        }
        return value;
    }
    static size_t size(const std::string& name) {
        struct stat s{};
        return stat(name.c_str(), &s) == 0 ? static_cast<size_t>(s.st_size) : 0;
    }
    static bool syncClose(FILE* file) {
        bool ok = fflush(file) == 0 && fsync(fileno(file)) == 0;
        return fclose(file) == 0 && ok;
    }
    bool fault() { failed_ = true; return false; }
    static bool queueName(const char* name, uint64_t* number = nullptr) {
        if (std::strlen(name) != 22 || name[0] != 'q' || std::strcmp(name + 17, ".gmfj")) return false;
        for (unsigned i = 1; i < 17; ++i)
            if (!((name[i] >= '0' && name[i] <= '9') || (name[i] >= 'a' && name[i] <= 'f'))) return false;
        if (number) *number = std::strtoull(name + 1, nullptr, 16);
        return true;
    }
    std::string oldest() const {
        DIR* dir = opendir(directory_.c_str());
        if (!dir) return {};
        std::string name;
        while (auto entry = readdir(dir))
            if (queueName(entry->d_name) && (name.empty() || name > entry->d_name)) name = entry->d_name;
        closedir(dir);
        return name.empty() ? name : directory_ + "/" + name;
    }
    bool header(FILE* file, FileHeader& h, std::string& metadata) {
        if (fseek(file, 0, SEEK_SET) || fread(&h, 1, sizeof(h), file) != sizeof(h) ||
            h.magic != FileMagic || h.version != 1 || !h.metadataBytes || h.metadataBytes > 2048 ||
            !h.sampleRate || h.sampleRate > 2000 || !h.channels || h.channels > 8 ||
            (h.sampleBits != 16 && h.sampleBits != 24)) return fault();
        metadata.resize(h.metadataBytes);
        if (fread(&metadata[0], 1, h.metadataBytes, file) != h.metadataBytes ||
            h.crc != crc(metadata.data(), metadata.size(), crc(&h, offsetof(FileHeader, crc)))) return fault();
        return true;
    }
    bool create(const SampleBlock& block, const std::string& metadata) {
        char name[32];
        std::snprintf(name, sizeof(name), "q%016llx.gmfj", static_cast<unsigned long long>(next_++));
        std::string path = directory_ + "/" + name;
        const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd < 0) return fault();
        FILE* file = fdopen(fd, "wb");
        if (!file) { close(fd); return fault(); }
        FileHeader h{FileMagic, 1, static_cast<uint16_t>(metadata.size()), block.sampleRate,
                     block.channels, block.sampleBits, 0};
        h.crc = crc(metadata.data(), metadata.size(), crc(&h, offsetof(FileHeader, crc)));
        bool ok = fwrite(&h, 1, sizeof(h), file) == sizeof(h) &&
            fwrite(metadata.data(), 1, metadata.size(), file) == metadata.size();
        ok = syncClose(file) && ok;
        if (!ok) return fault();
        writing_ = path; writingMetadata_ = metadata; used_ += sizeof(h) + metadata.size();
        queuedBytes_ += sizeof(h) + metadata.size();
        return true;
    }
    bool finishRead(bool incomplete) {
        if (reading_ == writing_) return false;
        const auto bytes = size(reading_);
        // Preserve an interrupted append as evidence; never silently format it.
        if (incomplete) {
            if (rename(reading_.c_str(), (reading_ + ".incomplete").c_str())) return fault();
            ++incompleteRecovered_;
        } else {
            if (unlink(reading_.c_str())) return fault();
            used_ -= bytes;
        }
        queuedBytes_ -= bytes;
        reading_.clear(); offset_ = 0;
        return true;
    }
public:
    bool begin(const char* directory, size_t maximumBytes) {
        if (!directory_.empty() || !directory || maximumBytes <= 2 * ReserveBytes) return false;
        directory_ = directory; maximum_ = maximumBytes;
        if (mkdir(directory, 0700) && errno != EEXIST) return fault();
        DIR* dir = opendir(directory);
        if (!dir) return fault();
        while (auto entry = readdir(dir)) {
            if (!std::strcmp(entry->d_name, ".") || !std::strcmp(entry->d_name, "..")) continue;
            const std::string path = directory_ + "/" + entry->d_name;
            used_ += size(path); // Includes retained incomplete records in the budget.
            if (queueName(entry->d_name)) queuedBytes_ += size(path);
            uint64_t number;
            std::string original(entry->d_name);
            if (original.size() > 22 && (original.substr(22) == ".incomplete" ||
                original.substr(22) == ".bench-preserved")) original.resize(22);
            if (queueName(original.c_str(), &number)) next_ = std::max(next_, number + 1);
        }
        closedir(dir);
        // Existing files are never appended after reboot: a torn suffix remains isolated.
        return !failed_;
    }
    bool append(const SampleBlock& block, const std::string& metadata) {
        if (failed_ || !block.valid() || metadata.empty() || metadata.size() > 2048) return false;
        const size_t bytes = sizeof(RecordHeader) + block.byteCount();
        if (used_ + bytes + sizeof(FileHeader) + metadata.size() + ReserveBytes > maximum_) return false;
        if (writing_.empty() || metadata != writingMetadata_ || size(writing_) + bytes > SegmentBytes)
            if (!create(block, metadata)) return false;
        const size_t at = size(writing_);
        RecordHeader h{RecordMagic, static_cast<uint16_t>(block.byteCount()), block.frames,
                       block.sequence, block.firstFrame, 0};
        h.crc = crc(block.payload, block.byteCount(), crc(&h, offsetof(RecordHeader, crc)));
        FILE* file = fopen(writing_.c_str(), "ab");
        if (!file) return fault();
        bool ok = fwrite(&h, 1, sizeof(h), file) == sizeof(h) &&
            fwrite(block.payload, 1, block.byteCount(), file) == block.byteCount();
        ok = syncClose(file) && ok;
        if (!ok) return fault();
        used_ += bytes; queuedBytes_ += bytes;
        // RAM ownership transfers only after close and an independent readback.
        file = fopen(writing_.c_str(), "rb");
        if (!file) return fault();
        RecordHeader saved{}; uint8_t returned[SampleBlock::MaxBytes];
        FileHeader fileHeader{}; std::string savedMetadata;
        ok = header(file, fileHeader, savedMetadata) && savedMetadata == metadata && !fseek(file, at, SEEK_SET) && fread(&saved, 1, sizeof(saved), file) == sizeof(saved) &&
            !std::memcmp(&saved, &h, sizeof(h)) && fread(returned, 1, h.bytes, file) == h.bytes &&
            !std::memcmp(returned, block.payload, h.bytes);
        fclose(file);
        return ok || fault();
    }
    bool peek(SampleBlock& block, std::string& metadata) {
        if (failed_ || pending_) return false;
        for (unsigned skipped = 0; skipped < 256; ++skipped) {
            if (reading_.empty()) { reading_ = oldest(); offset_ = 0; }
            if (reading_.empty()) return false;
            FILE* file = fopen(reading_.c_str(), "rb");
            if (!file) return fault();
            FileHeader fh{}; std::string base;
            const size_t observed = size(reading_);
            const bool partialHeader = observed < sizeof(fh) ||
                (fread(&fh, 1, sizeof(fh), file) == sizeof(fh) &&
                 fh.magic == FileMagic && fh.version == 1 && fh.metadataBytes <= 2048 &&
                 observed < sizeof(fh) + fh.metadataBytes);
            if (partialHeader) {
                fclose(file);
                if (finishRead(true)) continue;
                return false;
            }
            if (!header(file, fh, base)) { fclose(file); return false; }
            const size_t start = sizeof(fh) + fh.metadataBytes;
            if (!offset_) offset_ = start;
            const size_t end = size(reading_);
            if (offset_ == end) {
                fclose(file);
                if (finishRead(false)) continue;
                return false;
            }
            RecordHeader rh{};
            if (offset_ > end || fseek(file, offset_, SEEK_SET)) { fclose(file); return fault(); }
            if (end - offset_ < sizeof(rh)) {
                fclose(file);
                if (finishRead(true)) continue;
                return false;
            }
            if (fread(&rh, 1, sizeof(rh), file) != sizeof(rh) || rh.magic != RecordMagic ||
                !rh.bytes || rh.bytes > SampleBlock::MaxBytes) { fclose(file); return fault(); }
            if (end - offset_ - sizeof(rh) < rh.bytes) {
                fclose(file);
                if (finishRead(true)) continue;
                return false;
            }
            block.capture = CaptureMetadata{};
            block.sequence = rh.sequence; block.firstFrame = rh.firstFrame; block.frames = rh.frames;
            block.sampleRate = fh.sampleRate; block.channels = fh.channels; block.sampleBits = fh.sampleBits;
            bool ok = block.valid() && block.byteCount() == rh.bytes &&
                fread(block.payload, 1, rh.bytes, file) == rh.bytes &&
                rh.crc == crc(block.payload, rh.bytes, crc(&rh, offsetof(RecordHeader, crc)));
            fclose(file);
            if (!ok) return fault();
            metadata = base; pendingMetadata_ = base;
            pendingSequence_ = rh.sequence; nextOffset_ = offset_ + sizeof(rh) + rh.bytes;
            pending_ = true;
            return true;
        }
        return false;
    }
    // Called exclusively after device/session/sequence/payload-SHA ACK verification.
    bool acknowledge(uint64_t sequence) {
        if (!pending_ || pendingSequence_ != sequence || failed_) return false;
        offset_ = nextOffset_; pending_ = false; pendingMetadata_.clear();
        return true;
    }
    size_t used() const { return used_; }
    size_t pendingBytes() const { return queuedBytes_ > offset_ ? queuedBytes_ - offset_ : 0; }
    size_t capacity() const { return maximum_ > ReserveBytes ? maximum_ - ReserveBytes : 0; }
    bool failed() const { return failed_; }
    uint32_t incompleteRecovered() const { return incompleteRecovered_; }
};
} // namespace greenmind
