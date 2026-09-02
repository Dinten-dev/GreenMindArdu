#pragma once

#include <Arduino.h>
#include "measurement_batch.h"

class SensorSpool {
  public:
    bool begin();
    bool append(const MeasurementBatch& batch);
    bool peek(MeasurementBatch& batch);
    bool acknowledge();
    bool hasPending() const;
    size_t pendingRecords() const;
    size_t usedBytes() const;
    size_t totalBytes() const;

  private:
    bool mounted_ = false;
    uint32_t generation_ = 0;
    uint32_t nextSegment_ = 0;
    String oldestPath_;
    size_t oldestOffset_ = 0;
    size_t pendingRecords_ = 0;

    bool scan();
    bool findOldest(String& path) const;
    String currentPath() const;
};

extern SensorSpool sensorSpool;
