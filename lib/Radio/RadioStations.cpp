#include "RadioStations.h"

#if CROSSPOINT_CAP_RADIO

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "FsHelpers.h"

namespace {

// One list file is read whole into PSRAM: the parser wants a contiguous buffer,
// and 512 KB covers ~4000 entries in the MacRadio JSON layout. Anything bigger
// is treated as a malformed list.
constexpr size_t kMaxListBytes = 512 * 1024;

void copyField(char* dst, const size_t cap, const char* src) {
  if (!dst || cap == 0) return;
  dst[0] = '\0';
  if (!src) return;
  size_t len = strlen(src);
  if (len >= cap) {
    len = cap - 1;
    while (len > 0 && (static_cast<uint8_t>(src[len]) & 0xC0) == 0x80) --len;  // no split UTF-8
  }
  memcpy(dst, src, len);
  dst[len] = '\0';
}

std::string_view trimView(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
  return s;
}

// "my_stations.m3u" -> "my_stations", used as the default category of a file.
std::string fileStem(const char* name) {
  std::string_view view(name);
  const size_t dot = view.find_last_of('.');
  if (dot != std::string_view::npos) view = view.substr(0, dot);
  return std::string(view);
}

bool hasExtension(const char* name, const char* ext) {
  const std::string_view view(name);
  const size_t len = strlen(ext);
  if (view.size() < len) return false;
  const std::string_view tail = view.substr(view.size() - len);
  return std::equal(tail.begin(), tail.end(), ext, [](const char a, const char b) {
    return tolower(static_cast<unsigned char>(a)) == tolower(static_cast<unsigned char>(b));
  });
}

}  // namespace

void RadioStations::clear() {
  slots_.reset();
  items_ = nullptr;
  count_ = 0;
}

void RadioStations::append(const char* name, const char* url, const char* category) {
  if (!name || !url || !items_ || count_ >= kMaxStations) return;
  if (name[0] == '\0' || url[0] == '\0') return;
  RadioStation& slot = items_[count_];
  copyField(slot.name, sizeof(slot.name), name);
  copyField(slot.url, sizeof(slot.url), url);
  copyField(slot.category, sizeof(slot.category), category && category[0] ? category : "Radio");
  if (slot.name[0] == '\0' || slot.url[0] == '\0') return;
  ++count_;
}

void RadioStations::parseJson(char* text, const size_t len, const char* fallbackCategory) {
  JsonDocument doc;
  if (deserializeJson(doc, text, len) != DeserializationError::Ok) {
    LOG_ERR("RAD", "Station JSON is malformed");
    return;
  }
  const JsonArray stations = doc.as<JsonArray>();
  if (stations.isNull()) {
    LOG_ERR("RAD", "Station JSON is not an array");
    return;
  }
  for (const JsonObject station : stations) {
    if (count_ >= kMaxStations) break;
    const char* name = station["name"] | "";
    const char* url = station["url"] | "";
    const char* category = station["category"] | fallbackCategory;
    append(name, url, category);
  }
}

void RadioStations::parseM3u(const char* text, const size_t len, const char* category) {
  char pendingName[64] = {};
  size_t pos = 0;
  while (pos < len && count_ < kMaxStations) {
    const size_t start = pos;
    while (pos < len && text[pos] != '\n') ++pos;
    std::string_view line(text + start, pos - start);
    ++pos;  // skip '\n'
    line = trimView(line);
    if (line.empty()) continue;

    if (line.front() == '#') {
      // #EXTINF:-1 tvg-name="..." ,Display name
      const size_t comma = line.find(',');
      if (line.rfind("#EXTINF", 0) == 0 && comma != std::string_view::npos) {
        const std::string_view display = trimView(line.substr(comma + 1));
        copyField(pendingName, sizeof(pendingName), std::string(display).c_str());
      }
      continue;
    }
    const std::string url(line);
    // A bare URL without EXTINF still gets a usable label: the host name.
    append(pendingName[0] ? pendingName : url.c_str(), url.c_str(), category);
    pendingName[0] = '\0';
  }
}

void RadioStations::parseTxt(const char* text, const size_t len, const char* category) {
  size_t pos = 0;
  while (pos < len && count_ < kMaxStations) {
    const size_t start = pos;
    while (pos < len && text[pos] != '\n') ++pos;
    std::string_view line(text + start, pos - start);
    ++pos;
    line = trimView(line);
    if (line.empty() || line.front() == '#') continue;

    // "Name,URL" — the comma separator is the reference app's txt format.
    const size_t comma = line.find(',');
    if (comma == std::string_view::npos) continue;
    const std::string name(trimView(line.substr(0, comma)));
    const std::string url(trimView(line.substr(comma + 1)));
    append(name.c_str(), url.c_str(), category);
  }
}

RadioStations::LoadResult RadioStations::load() {
  clear();
  if (!Storage.ready()) return LoadResult::NoSd;

  std::vector<std::string> files;
  {
    HalStorage::StorageLock lock;
    HalFile dir = Storage.open(kFolder);
    if (!dir.isOpen() || !dir.isDirectory()) return LoadResult::NoFolder;

    char name[256];
    for (HalFile entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
      if (entry.isDirectory()) continue;
      if (entry.getName(name, sizeof(name)) == 0) continue;
      if (name[0] == '\0' || name[0] == '.') continue;
      if (!hasExtension(name, ".json") && !hasExtension(name, ".m3u") && !hasExtension(name, ".txt")) continue;
      files.emplace_back(name);
    }
  }
  if (files.empty()) return LoadResult::NoFiles;
  FsHelpers::sortFileList(files);

  // ~280 KB of station records for 1024 entries, in PSRAM: the table scales
  // with the user's list while internal RAM stays available for the decoder
  // and the TLS session; freed by clear().
  slots_ = HalMemory::allocatePsram(kMaxStations * sizeof(RadioStation));
  if (!slots_) {
    LOG_ERR("RAD", "No PSRAM for %u station slots", static_cast<unsigned>(kMaxStations));
    return LoadResult::NoFiles;
  }
  // RadioStation holds char arrays only, so any malloc alignment is sufficient.
  items_ = reinterpret_cast<RadioStation*>(slots_.get());

  // One PSRAM scratch buffer reused for every file; +1 for the reader's NUL.
  HalMemory::PsramBuffer fileBuf = HalMemory::allocatePsram(kMaxListBytes + 1);
  if (!fileBuf) {
    LOG_ERR("RAD", "No PSRAM for the %u-byte list buffer", static_cast<unsigned>(kMaxListBytes));
    clear();
    return LoadResult::NoFiles;
  }
  char* const buf = reinterpret_cast<char*>(fileBuf.get());

  for (const std::string& file : files) {
    if (count_ >= kMaxStations) break;
    std::string path(kFolder);
    path += '/';
    path += file;
    const size_t len = Storage.readFileToBuffer(path.c_str(), buf, kMaxListBytes + 1, kMaxListBytes);
    if (len == 0) {
      LOG_ERR("RAD", "Cannot read %s", path.c_str());
      continue;
    }
    if (hasExtension(file.c_str(), ".json")) {
      parseJson(buf, len, nullptr);
    } else if (hasExtension(file.c_str(), ".m3u")) {
      const std::string category = fileStem(file.c_str());
      parseM3u(buf, len, category.c_str());
    } else {
      const std::string category = fileStem(file.c_str());
      parseTxt(buf, len, category.c_str());
    }
    LOG_INF("RAD", "%s -> %u stations", file.c_str(), static_cast<unsigned>(count_));
  }

  if (count_ == 0) {
    clear();
    return LoadResult::NoFiles;
  }
  LOG_INF("RAD", "Loaded %u stations from %u files", static_cast<unsigned>(count_),
          static_cast<unsigned>(files.size()));
  return LoadResult::Ok;
}

#endif  // CROSSPOINT_CAP_RADIO
