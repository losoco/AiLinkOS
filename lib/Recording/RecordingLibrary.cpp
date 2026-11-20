#include "RecordingLibrary.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace recording {
namespace {

constexpr size_t kWavHeaderBytes = 44;

bool endsWithWav(const char* name) {
  const size_t len = strlen(name);
  if (len < 4) return false;
  return tolower(static_cast<unsigned char>(name[len - 4])) == '.' &&
         tolower(static_cast<unsigned char>(name[len - 3])) == 'w' &&
         tolower(static_cast<unsigned char>(name[len - 2])) == 'a' &&
         tolower(static_cast<unsigned char>(name[len - 1])) == 'v';
}

bool readEntry(const char* name, RecordingEntry& entry) {
  snprintf(entry.path, sizeof(entry.path), "/rec/%s", name);
  const size_t nameLen = strlen(name);
  snprintf(entry.name, sizeof(entry.name), "%.*s", static_cast<int>(nameLen >= 4 ? nameLen - 4 : nameLen), name);

  uint8_t header[kWavHeaderBytes];
  HalFile file;
  if (!Storage.openFileForRead("REC", entry.path, file)) return false;
  if (file.read(header, sizeof(header)) != static_cast<int>(sizeof(header))) return false;
  if (memcmp(header, "RIFF", 4) != 0 || memcmp(header + 8, "WAVE", 4) != 0) return false;

  uint16_t channels = 0;
  uint32_t sampleRate = 0;
  uint32_t dataBytes = 0;
  memcpy(&channels, header + 22, sizeof(channels));
  memcpy(&sampleRate, header + 24, sizeof(sampleRate));
  memcpy(&dataBytes, header + 40, sizeof(dataBytes));
  if ((channels != 1 && channels != 2) || sampleRate < 8000 || sampleRate > 48000 || dataBytes == 0) return false;

  entry.sampleRate = static_cast<uint16_t>(sampleRate);
  entry.dataBytes = dataBytes;
  entry.durationMs = static_cast<uint32_t>(static_cast<uint64_t>(dataBytes) * 1000 /
                                           (static_cast<uint64_t>(sampleRate) * channels * 2));
  return true;
}

}  // namespace

bool scan(std::vector<RecordingEntry>& out, const size_t maxEntries) {
  out.clear();
  if (!Storage.ready()) return false;

  const std::vector<String> names = Storage.listFiles("/rec", static_cast<int>(maxEntries));
  out.reserve(std::min(names.size(), maxEntries));
  for (const String& name : names) {
    if (!endsWithWav(name.c_str())) continue;
    RecordingEntry entry;
    if (!readEntry(name.c_str(), entry)) {
      LOG_DBG("REC", "Skipping %s (not a recording)", name.c_str());
      continue;
    }
    out.push_back(entry);
    if (out.size() >= maxEntries) break;
  }

  // Timestamped names sort chronologically, so descending is newest-first.
  // Non-conforming names (legacy or debug files) sort after every REC_* entry.
  std::sort(out.begin(), out.end(), [](const RecordingEntry& a, const RecordingEntry& b) {
    const bool aRec = strncmp(a.name, "REC_", 4) == 0;
    const bool bRec = strncmp(b.name, "REC_", 4) == 0;
    if (aRec != bRec) return aRec;
    return strcmp(a.name, b.name) > 0;
  });
  return true;
}

bool remove(const char* path) { return Storage.remove(path); }

void formatDuration(const uint32_t ms, char* out, const size_t outLen) {
  const uint32_t totalSeconds = ms / 1000;
  const uint32_t hours = totalSeconds / 3600;
  const uint32_t minutes = (totalSeconds / 60) % 60;
  const uint32_t seconds = totalSeconds % 60;
  if (hours > 0) {
    snprintf(out, outLen, "%u:%02u:%02u", static_cast<unsigned>(hours), static_cast<unsigned>(minutes),
             static_cast<unsigned>(seconds));
  } else {
    snprintf(out, outLen, "%02u:%02u", static_cast<unsigned>(minutes), static_cast<unsigned>(seconds));
  }
}

void formatStartTime(const char* name, char* out, const size_t outLen) {
  // REC_YYYYMMDD_HHMMSS
  if (name != nullptr && strlen(name) >= 19 && strncmp(name, "REC_", 4) == 0) {
    snprintf(out, outLen, "%.2s-%.2s %.2s:%.2s", name + 8, name + 10, name + 13, name + 15);
    return;
  }
  snprintf(out, outLen, "%s", name != nullptr ? name : "");
}

}  // namespace recording
