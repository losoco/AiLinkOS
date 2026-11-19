#pragma once

#if CROSSPOINT_CAP_RADIO

#include <HalMemory.h>

#include <cstddef>
#include <cstdint>
#include <memory>

// One entry of the radio list imported from the SD card.
//
// Fixed-size fields keep a full list in one contiguous allocation and make the
// per-station cost predictable: 280 bytes each. The table lives in PSRAM
// (1024 stations * 280 B ~= 280 KB) so a large imported list never competes
// with the audio decoder and TLS session for internal RAM.
struct RadioStation {
  char name[64] = {};
  char url[192] = {};
  char category[24] = {};
};

// Loads /radio into memory. Accepted files, all optional and merged in name
// order:
//   stations.json  [{"category","name","url"}, ...]  (MacRadio schema)
//   *.m3u          #EXTINF:-1,Name + URL lines; category = file stem
//   *.txt          "Name,URL" per line;          category = file stem
//
// Only MP3-over-HTTP(S) streams are playable; HLS (.m3u8) entries are kept in
// the list but reported as unsupported when played, instead of being dropped
// silently.
class RadioStations {
 public:
  enum class LoadResult : uint8_t { Ok, NoSd, NoFolder, NoFiles };

  static constexpr const char* kFolder = "/radio";
  static constexpr size_t kMaxStations = 1024;

  RadioStations() = default;
  RadioStations(const RadioStations&) = delete;
  RadioStations& operator=(const RadioStations&) = delete;

  LoadResult load();
  void clear();

  size_t count() const { return count_; }
  const RadioStation& at(size_t index) const { return items_[index]; }
  bool empty() const { return count_ == 0; }

 private:
  void append(const char* name, const char* url, const char* category);
  // Mutable input: ArduinoJson stores zero-copy string views into it, so a
  // 1000-entry list costs nodes only instead of copying every name and URL.
  void parseJson(char* text, size_t len, const char* fallbackCategory);
  void parseM3u(const char* text, size_t len, const char* category);
  void parseTxt(const char* text, size_t len, const char* category);

  HalMemory::PsramBuffer slots_;
  RadioStation* items_ = nullptr;
  size_t count_ = 0;
};

#endif  // CROSSPOINT_CAP_RADIO
