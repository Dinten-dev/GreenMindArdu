#pragma once
#ifdef GREENMIND_DIRECT_DURABLE_SPOOL
#include <LittleFS.h>
#include <esp_partition.h>
#include "../../../direct_transport/FlashJournal.h"

static greenmind::FlashJournal directFlash;
static SemaphoreHandle_t directFlashMutex = nullptr;
static std::atomic<uint32_t> directFlashBytes{0}, directFlashCapacity{0}, directFlashPartial{0}, directFlashPendingBytes{0};
static std::atomic<bool> directFlashFault{false};
static std::atomic<uint32_t> directFlashCommitted{0}, directFlashLastSequence32{0};
static SampleBlock directFlashStaged;

static void directFlashSnapshot() {
    directFlashPendingBytes.store(directFlash.pendingBytes());
    directFlashBytes.store(directFlash.used()); directFlashCapacity.store(directFlash.capacity());
    directFlashFault.store(directFlash.failed()); directFlashPartial.store(directFlash.incompleteRecovered());
}
static bool verifyDirectFlashOwner() {
    const std::string expected = std::string("greenmind-direct-journal-v1|") +
        greenmind::cloud::host + "|" + deviceId.c_str() + "\n";
    FILE* f = fopen("/direct-spool/owner", "rb");
    if (f) {
        char bytes[256]; const size_t n = fread(bytes, 1, sizeof(bytes), f); fclose(f);
        return n == expected.size() && !std::memcmp(bytes, expected.data(), n);
    }
    // Missing ownership may be initialized only on an empty filesystem.
    DIR* dir = opendir("/direct-spool"); if (!dir) return false;
    bool empty = true;
    while (auto e = readdir(dir)) if (std::strcmp(e->d_name, ".") && std::strcmp(e->d_name, "..")) empty = false;
    closedir(dir); if (!empty) return false;
    const int fd = open("/direct-spool/owner", O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return false;
    f = fdopen(fd, "wb"); if (!f) { close(fd); return false; }
    bool ok = fwrite(expected.data(), 1, expected.size(), f) == expected.size();
    ok = fflush(f) == 0 && fsync(fileno(f)) == 0 && ok;
    const bool closed = fclose(f) == 0;
    return closed && ok;
}
static bool prepareDirectFlashSpool() {
    if (!LittleFS.begin(false, "/direct-spool", 10, "directspool")) {
#ifdef GREENMIND_DIRECT_INIT_ERASED_SPOOL
        // Only a completely erased, explicitly named new partition may be formatted.
        // A damaged/unknown existing filesystem must remain untouched.
        const auto* p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
            ESP_PARTITION_SUBTYPE_ANY, "directspool");
        if (!p) return false;
        uint8_t bytes[512];
        for (size_t at = 0; at < p->size; at += sizeof(bytes)) {
            if (esp_partition_read(p, at, bytes, sizeof(bytes)) != ESP_OK) return false;
            for (const auto value : bytes) if (value != 0xff) return false;
        }
        // begin(false) sets this instance's partition label even if mount fails.
        if (!LittleFS.format() || !LittleFS.begin(false, "/direct-spool", 10, "directspool")) return false;
        Serial.println("direct_flash_initialized erased_partition_only=1");
#else
        return false;
#endif
    }
    if (!verifyDirectFlashOwner()) return false;
    directFlashMutex = xSemaphoreCreateMutex();
    if (!directFlashMutex || !directFlash.begin("/direct-spool/queue", LittleFS.totalBytes())) return false;
    directFlashSnapshot();
    Serial.printf("direct_flash_ready capacity=%u occupied=%u durable=1 auto_format_existing=0\n",
        directFlashCapacity.load(), directFlashBytes.load());
    return true;
}
static void directFlashWriter(void*) {
    for (;;) {
        if (!directQueue.peek(directFlashStaged)) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
        const String frozen = frozenDirectMetadata(directFlashStaged);
        const std::string metadata(frozen.c_str(), frozen.length());
        xSemaphoreTake(directFlashMutex, portMAX_DELAY);
        const auto available = LittleFS.totalBytes() - LittleFS.usedBytes();
        const bool saved = available > 65536 + 2 * directFlashStaged.byteCount() &&
            directFlash.append(directFlashStaged, metadata);
        directFlashSnapshot();
        xSemaphoreGive(directFlashMutex);
        if (saved) {
            directFlashCommitted.fetch_add(1);
            directFlashLastSequence32.store(static_cast<uint32_t>(directFlashStaged.sequence));
        }
        if (saved && !directQueue.acknowledge(directFlashStaged.sequence)) {
            directFlashFault.store(true);
            Serial.println("direct_flash_ram_ack_invariant_failed retained=1");
            vTaskDelete(nullptr);
        }
        if (!saved) vTaskDelay(pdMS_TO_TICKS(50));
    }
}
static bool directFlashPeek(SampleBlock& block, std::string& metadata) {
    xSemaphoreTake(directFlashMutex, portMAX_DELAY);
    const bool ready = directFlash.peek(block, metadata);
    directFlashSnapshot();
    xSemaphoreGive(directFlashMutex);
    return ready;
}
static bool directFlashAcknowledge(uint64_t sequence) {
    xSemaphoreTake(directFlashMutex, portMAX_DELAY);
    const bool acknowledged = directFlash.acknowledge(sequence);
    directFlashSnapshot();
    xSemaphoreGive(directFlashMutex);
    return acknowledged;
}
static void printDirectFlashStats() {
    Serial.printf("direct_flash_used=%u pending_bytes=%u capacity=%u failed=%d partial_files_retained=%u committed_this_boot=%u last_sequence32=%u durable=1\n",
        directFlashBytes.load(), directFlashPendingBytes.load(), directFlashCapacity.load(), directFlashFault.load(), directFlashPartial.load(),
        directFlashCommitted.load(), directFlashLastSequence32.load());
}
#else
static void printDirectFlashStats() {}
#endif
