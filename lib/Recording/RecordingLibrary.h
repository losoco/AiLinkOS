#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// One entry in the /rec list. Fixed-size storage keeps the whole listing in a
// small reserved vector instead of one heap string per field.
struct RecordingEntry {
  char name[40] = {};  // file name without extension, e.g. REC_20261120_143210
  char path[48] = {};
  uint32_t dataBytes = 0;
  uint32_t durationMs = 0;
  uint16_t sampleRate = 0;
};

namespace recording {

// Readings are newest-first. Returns false when the card is not mounted.
bool scan(std::vector<RecordingEntry>& out, size_t maxEntries = 200);
bool remove(const char* path);

// "MM:SS" below an hour, "H:MM:SS" above.
void formatDuration(uint32_t ms, char* out, size_t outLen);
// "REC_20261120_143210" -> "11-20 14:32"; falls back to the raw name.
void formatStartTime(const char* name, char* out, size_t outLen);

}  // namespace recording
