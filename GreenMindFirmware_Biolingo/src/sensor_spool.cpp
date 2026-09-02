#include "sensor_spool.h"

#include <LittleFS.h>
#include <Preferences.h>

namespace {

constexpr uint32_t RECORD_MAGIC = 0x314D5147; // GQM1
constexpr uint16_t RECORD_VERSION = 1;
constexpr size_t RECORDS_PER_SEGMENT = 64;
constexpr char SPOOL_DIR[] = "/spool";

struct __attribute__((packed)) PersistedRecord {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    MeasurementBatch batch;
    uint32_t crc32;
};

uint32_t crc32(const uint8_t* data, size_t length) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t index = 0; index < length; ++index) {
        crc ^= data[index];
        for (uint8_t bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
        }
    }
    return ~crc;
}

bool isSegmentName(const String& path) {
    return path.startsWith(String(SPOOL_DIR) + "/g") && path.endsWith(".gmq");
}

} // namespace

SensorSpool sensorSpool;

bool SensorSpool::begin() {
    Preferences metadata;
    metadata.begin("gm_spool", false);
    bool initialized = metadata.getBool("initialized", false);

    if (!LittleFS.begin(false)) {
        if (initialized) {
            Serial.println("[Spool] LittleFS mount failed; retained without formatting");
            metadata.end();
            return false;
        }
        Serial.println("[Spool] Initializing new LittleFS partition");
        if (!LittleFS.format() || !LittleFS.begin(false)) {
            Serial.println("[Spool] LittleFS initialization failed");
            metadata.end();
            return false;
        }
        metadata.putBool("initialized", true);
    }
    metadata.putBool("initialized", true);

    generation_ = metadata.getUInt("generation", 0) + 1;
    metadata.putUInt("generation", generation_);
    metadata.end();

    if (!LittleFS.exists(SPOOL_DIR) && !LittleFS.mkdir(SPOOL_DIR)) {
        Serial.println("[Spool] Could not create spool directory");
        LittleFS.end();
        return false;
    }

    mounted_ = true;
    if (!scan()) {
        mounted_ = false;
        LittleFS.end();
        return false;
    }
    Serial.printf("[Spool] Ready: %u pending, %u/%u bytes used\n",
                  static_cast<unsigned>(pendingRecords_), static_cast<unsigned>(usedBytes()),
                  static_cast<unsigned>(totalBytes()));
    return true;
}

String SensorSpool::currentPath() const {
    char path[48];
    snprintf(path, sizeof(path), "%s/g%010lu-s%06lu.gmq", SPOOL_DIR,
             static_cast<unsigned long>(generation_), static_cast<unsigned long>(nextSegment_));
    return String(path);
}

bool SensorSpool::append(const MeasurementBatch& batch) {
    if (!mounted_)
        return false;

    String path = currentPath();
    File file = LittleFS.open(path, FILE_APPEND);
    if (!file) {
        Serial.println("[Spool] Could not open segment for append");
        return false;
    }

    if (file.size() >= RECORDS_PER_SEGMENT * sizeof(PersistedRecord)) {
        file.close();
        ++nextSegment_;
        path = currentPath();
        file = LittleFS.open(path, FILE_APPEND);
        if (!file)
            return false;
    }

    PersistedRecord record{};
    record.magic = RECORD_MAGIC;
    record.version = RECORD_VERSION;
    record.size = sizeof(PersistedRecord);
    record.batch = batch;
    record.crc32 = crc32(reinterpret_cast<const uint8_t*>(&record),
                         sizeof(PersistedRecord) - sizeof(record.crc32));

    size_t written = file.write(reinterpret_cast<const uint8_t*>(&record), sizeof(record));
    file.flush();
    file.close();
    if (written != sizeof(record)) {
        Serial.println("[Spool] Short write; record not acknowledged");
        ++nextSegment_;
        return false;
    }

    ++pendingRecords_;
    if (oldestPath_.isEmpty()) {
        oldestPath_ = path;
        oldestOffset_ = 0;
    }
    return true;
}

bool SensorSpool::findOldest(String& path) const {
    File directory = LittleFS.open(SPOOL_DIR);
    if (!directory || !directory.isDirectory())
        return false;

    String candidate;
    for (File entry = directory.openNextFile(); entry; entry = directory.openNextFile()) {
        String name = entry.name();
        if (!name.startsWith("/"))
            name = String(SPOOL_DIR) + "/" + name;
        if (!entry.isDirectory() && isSegmentName(name) &&
            entry.size() >= sizeof(PersistedRecord) &&
            (candidate.isEmpty() || name < candidate))
            candidate = name;
        entry.close();
    }
    directory.close();
    if (candidate.isEmpty())
        return false;
    path = candidate;
    return true;
}

bool SensorSpool::peek(MeasurementBatch& batch) {
    if (!mounted_ || pendingRecords_ == 0)
        return false;
    if (oldestPath_.isEmpty() && !findOldest(oldestPath_))
        return false;

    File file = LittleFS.open(oldestPath_, FILE_READ);
    if (!file || !file.seek(oldestOffset_)) {
        if (file)
            file.close();
        return false;
    }

    PersistedRecord record{};
    size_t bytesRead = file.read(reinterpret_cast<uint8_t*>(&record), sizeof(record));
    file.close();
    if (bytesRead != sizeof(record) || record.magic != RECORD_MAGIC ||
        record.version != RECORD_VERSION || record.size != sizeof(PersistedRecord)) {
        Serial.printf("[Spool] Invalid record retained at %s:%u\n", oldestPath_.c_str(),
                      static_cast<unsigned>(oldestOffset_));
        return false;
    }
    uint32_t expected = crc32(reinterpret_cast<const uint8_t*>(&record),
                              sizeof(PersistedRecord) - sizeof(record.crc32));
    if (expected != record.crc32) {
        Serial.printf("[Spool] CRC mismatch retained at %s:%u\n", oldestPath_.c_str(),
                      static_cast<unsigned>(oldestOffset_));
        return false;
    }
    batch = record.batch;
    return true;
}

bool SensorSpool::acknowledge() {
    if (!mounted_ || pendingRecords_ == 0 || oldestPath_.isEmpty())
        return false;

    oldestOffset_ += sizeof(PersistedRecord);
    --pendingRecords_;
    File file = LittleFS.open(oldestPath_, FILE_READ);
    size_t fileSize = file ? file.size() : 0;
    if (file)
        file.close();

    if (oldestOffset_ >= fileSize || fileSize - oldestOffset_ < sizeof(PersistedRecord)) {
        String acknowledgedPath = oldestPath_;
        oldestPath_ = "";
        oldestOffset_ = 0;
        if (!LittleFS.remove(acknowledgedPath)) {
            Serial.printf("[Spool] Failed removing acknowledged segment %s\n",
                          acknowledgedPath.c_str());
            oldestPath_ = acknowledgedPath;
            oldestOffset_ -= sizeof(PersistedRecord);
            ++pendingRecords_;
            return false;
        }
        if (pendingRecords_ > 0)
            findOldest(oldestPath_);
    }
    return true;
}

bool SensorSpool::scan() {
    pendingRecords_ = 0;
    oldestPath_ = "";
    oldestOffset_ = 0;
    nextSegment_ = 0;

    File directory = LittleFS.open(SPOOL_DIR);
    if (!directory || !directory.isDirectory())
        return false;

    for (File entry = directory.openNextFile(); entry; entry = directory.openNextFile()) {
        String name = entry.name();
        if (!name.startsWith("/"))
            name = String(SPOOL_DIR) + "/" + name;
        if (!entry.isDirectory() && isSegmentName(name)) {
            size_t validBytes = (entry.size() / sizeof(PersistedRecord)) * sizeof(PersistedRecord);
            pendingRecords_ += validBytes / sizeof(PersistedRecord);
            if (validBytes > 0 && (oldestPath_.isEmpty() || name < oldestPath_))
                oldestPath_ = name;
        }
        entry.close();
    }
    directory.close();
    return true;
}

bool SensorSpool::hasPending() const {
    return pendingRecords_ > 0;
}

size_t SensorSpool::pendingRecords() const {
    return pendingRecords_;
}

size_t SensorSpool::usedBytes() const {
    return mounted_ ? LittleFS.usedBytes() : 0;
}

size_t SensorSpool::totalBytes() const {
    return mounted_ ? LittleFS.totalBytes() : 0;
}
