#pragma once

#if CROSSPOINT_CAP_MUSIC_PLAYER

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Standard LRC (lyrics) sidecar reader.
//
// Loads "<audio base name>.lrc" from the same directory as the track and keeps
// a sorted timestamp table plus one text blob. Supported syntax:
//
//   [mm:ss] / [mm:ss.x] / [mm:ss.xx] / [mm:ss.xxx]   timestamps
//   [00:12.00][00:45.00]chorus                       several stamps per line
//   [offset:+500] / [offset:-500]                    global shift in ms
//   [ti:...] [ar:...] [al:...] [by:...]              metadata (ignored)
//   <00:12.00>word                                   enhanced-LRC (stripped)
//
// Both the table and the text are heap-allocated with makeUniqueNoThrow and
// capped (kMaxLines / kTextCapacity) so a malformed or oversized file cannot
// exhaust the shared ESP32-C3 budget. Construction does not allocate; load()
// does, and clear() releases.
class LrcLyrics {
 public:
  // Parses the sidecar for `audioPath`. Returns false when there is no usable
  // .lrc next to it (missing file, empty table, OOM). Never throws.
  bool loadForTrack(const std::string& audioPath);

  // Releases the table and text blob. Safe to call repeatedly.
  void clear();

  bool empty() const { return entries_.empty(); }
  size_t size() const { return entries_.size(); }

  // Index of the last line whose timestamp is <= positionMs, or -1 before the
  // first timestamp.
  int indexAt(uint32_t positionMs) const;

  // NUL-terminated lyric text for `index` (empty string when out of range).
  const char* line(size_t index) const;

 private:
  struct Entry {
    uint32_t timeMs;
    uint16_t offset;
    uint16_t length;
  };

  std::vector<Entry> entries_;
  std::unique_ptr<char[]> text_;
  size_t textLength_ = 0;
  size_t textCapacity_ = 0;
};

#endif  // CROSSPOINT_CAP_MUSIC_PLAYER
