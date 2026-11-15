#include "LrcLyrics.h"

#if CROSSPOINT_CAP_MUSIC_PLAYER

#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace {

// A typical LRC is 40-80 lines / 1-3 KB. These caps keep the worst case at
// roughly 2 KB (table) + 4 KB (text), which is acceptable next to the player's
// cover snapshot and is fully released by clear().
constexpr size_t kMaxLines = 256;
constexpr size_t kTextCapacity = 4096;
constexpr size_t kLineCapacity = 512;
constexpr size_t kMaxStampsPerLine = 8;

bool isDigit(const char c) { return c >= '0' && c <= '9'; }

// "[mm:ss]" / "[mm:ss.xxx]" content -> milliseconds. Returns false for metadata
// tags such as "ti:..." or "offset:...".
bool parseTimestamp(const char* s, const size_t len, uint32_t& outMs) {
  size_t i = 0;
  uint32_t minutes = 0;
  size_t digits = 0;
  while (i < len && isDigit(s[i])) {
    minutes = minutes * 10 + static_cast<uint32_t>(s[i] - '0');
    ++i;
    ++digits;
  }
  if (digits == 0 || i >= len || s[i] != ':') return false;
  ++i;

  uint32_t seconds = 0;
  digits = 0;
  while (i < len && isDigit(s[i])) {
    seconds = seconds * 10 + static_cast<uint32_t>(s[i] - '0');
    ++i;
    ++digits;
  }
  if (digits == 0 || digits > 2) return false;

  uint32_t millis = 0;
  if (i < len && s[i] == '.') {
    ++i;
    size_t fracDigits = 0;
    while (i < len && isDigit(s[i]) && fracDigits < 3) {
      millis = millis * 10 + static_cast<uint32_t>(s[i] - '0');
      ++i;
      ++fracDigits;
    }
    if (fracDigits == 0) return false;
    while (fracDigits < 3) {
      millis *= 10;
      ++fracDigits;
    }
  }
  if (i != len) return false;

  outMs = (minutes * 60 + seconds) * 1000 + millis;
  return true;
}

bool readLine(HalFile& file, char* buffer, size_t capacity) {
  size_t used = 0;
  bool sawAny = false;
  int c;
  while ((c = file.read()) >= 0) {
    sawAny = true;
    if (c == '\n') break;
    if (used + 1 < capacity) buffer[used++] = static_cast<char>(c);
  }
  buffer[used] = '\0';
  return sawAny;
}

// Drops enhanced-LRC word timestamps ("<00:12.00>") and returns the new start.
char* stripWordTags(char* s) {
  char* write = s;
  for (char* read = s; *read != '\0';) {
    if (*read == '<') {
      while (*read != '\0' && *read != '>') ++read;
      if (*read == '>') ++read;
      continue;
    }
    *write++ = *read++;
  }
  *write = '\0';
  return s;
}

// Leading spaces/tabs and trailing spaces/CR removed in place.
char* trimText(char* s) {
  while (*s == ' ' || *s == '\t') ++s;
  size_t len = std::strlen(s);
  while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\r')) s[--len] = '\0';
  return s;
}

}  // namespace

bool LrcLyrics::loadForTrack(const std::string& audioPath) {
  clear();
  if (audioPath.empty()) return false;

  std::string lrcPath = audioPath;
  const size_t dot = lrcPath.find_last_of('.');
  const size_t slash = lrcPath.find_last_of('/');
  if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) lrcPath.resize(dot);
  lrcPath += ".lrc";

  HalFile file;
  if (!Storage.openFileForRead("LRC", lrcPath, file)) return false;

  text_ = makeUniqueNoThrow<char[]>(kTextCapacity);
  if (!text_) {
    LOG_ERR("LRC", "OOM: %u-byte text blob", static_cast<unsigned>(kTextCapacity));
    return false;
  }
  textCapacity_ = kTextCapacity;
  entries_.reserve(64);

  int offsetMs = 0;
  bool firstLine = true;
  char buffer[kLineCapacity];

  while (readLine(file, buffer, sizeof(buffer))) {
    char* cursor = buffer;
    if (firstLine) {
      firstLine = false;
      // UTF-8 BOM.
      if (static_cast<uint8_t>(cursor[0]) == 0xEF && static_cast<uint8_t>(cursor[1]) == 0xBB &&
          static_cast<uint8_t>(cursor[2]) == 0xBF) {
        cursor += 3;
      }
    }

    uint32_t stamps[kMaxStampsPerLine];
    size_t stampCount = 0;
    while (*cursor == '[') {
      char* close = std::strchr(cursor, ']');
      if (close == nullptr) break;
      const size_t tagLen = static_cast<size_t>(close - cursor - 1);
      uint32_t stamp = 0;
      if (parseTimestamp(cursor + 1, tagLen, stamp)) {
        if (stampCount < kMaxStampsPerLine) stamps[stampCount++] = stamp;
      } else if (tagLen > 7 && std::strncmp(cursor + 1, "offset:", 7) == 0) {
        offsetMs = std::atoi(cursor + 8);
      }
      cursor = close + 1;
    }

    char* text = trimText(stripWordTags(cursor));
    if (stampCount == 0 || text[0] == '\0') continue;

    const size_t textLen = std::strlen(text);
    if (textLength_ + textLen + 1 > textCapacity_) {
      LOG_DBG("LRC", "Text cap reached, dropping the remaining lines");
      break;
    }
    std::memcpy(text_.get() + textLength_, text, textLen);
    text_[textLength_ + textLen] = '\0';
    const uint16_t offset = static_cast<uint16_t>(textLength_);
    textLength_ += textLen + 1;

    for (size_t i = 0; i < stampCount && entries_.size() < kMaxLines; ++i) {
      entries_.push_back(Entry{stamps[i], offset, static_cast<uint16_t>(textLen)});
    }
  }

  if (entries_.empty()) {
    clear();
    return false;
  }

  // [offset:] may appear anywhere in the file, so it is applied after parsing.
  if (offsetMs != 0) {
    for (Entry& entry : entries_) {
      const int64_t shifted = static_cast<int64_t>(entry.timeMs) + offsetMs;
      entry.timeMs = shifted < 0 ? 0 : static_cast<uint32_t>(shifted);
    }
  }

  std::sort(entries_.begin(), entries_.end(), [](const Entry& a, const Entry& b) { return a.timeMs < b.timeMs; });

  LOG_INF("LRC", "Loaded %s: %u lines, %u text bytes", lrcPath.c_str(), static_cast<unsigned>(entries_.size()),
          static_cast<unsigned>(textLength_));
  return true;
}

void LrcLyrics::clear() {
  std::vector<Entry>().swap(entries_);
  text_.reset();
  textLength_ = 0;
  textCapacity_ = 0;
}

int LrcLyrics::indexAt(const uint32_t positionMs) const {
  int low = 0;
  int high = static_cast<int>(entries_.size()) - 1;
  int found = -1;
  while (low <= high) {
    const int mid = low + (high - low) / 2;
    if (entries_[static_cast<size_t>(mid)].timeMs <= positionMs) {
      found = mid;
      low = mid + 1;
    } else {
      high = mid - 1;
    }
  }
  return found;
}

const char* LrcLyrics::line(const size_t index) const {
  if (index >= entries_.size() || !text_) return "";
  return text_.get() + entries_[index].offset;
}

#endif  // CROSSPOINT_CAP_MUSIC_PLAYER
