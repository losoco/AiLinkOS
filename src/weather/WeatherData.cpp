#include "WeatherData.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <StreamingJsonParser.h>
#include <WiFi.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "network/HttpDownloader.h"
#include "util/TimeUtils.h"

namespace {

constexpr char kCachePath[] = "/.crosspoint/weather.txt";
constexpr char kCacheTmpPath[] = "/.crosspoint/weather.txt.tmp";
constexpr char kCacheDir[] = "/.crosspoint";
constexpr char kCityFile[] = "/city_code.txt";

constexpr char kAmapUrl[] = "http://restapi.amap.com/v3/ip?key=0113a13c88697dcea6a445584d535837";
constexpr char kSojsonPrefix[] = "http://t.weather.sojson.com/api/weather/city/";

constexpr size_t kLineMax = 128;

void copyField(char* dst, size_t cap, const char* src, size_t len) {
  if (cap == 0) return;
  const size_t n = (len < cap - 1) ? len : cap - 1;
  if (n > 0) std::memcpy(dst, src, n);
  dst[n] = '\0';
}

char* trimInPlace(char* s) {
  while (*s == ' ' || *s == '\t') ++s;
  size_t len = std::strlen(s);
  while (len > 0 && (s[len - 1] == '\r' || s[len - 1] == '\n' || s[len - 1] == ' ')) s[--len] = '\0';
  return s;
}

// Read one '\n'-terminated line. Returns false at EOF. The returned buffer is
// always NUL-terminated; over-long lines are truncated, not split.
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

// Split on `sep` keeping empty fields (strtok would swallow them).
int splitFields(char* s, char sep, char** out, int maxFields) {
  int count = 0;
  char* p = s;
  while (count < maxFields) {
    out[count++] = p;
    char* next = std::strchr(p, sep);
    if (next == nullptr) break;
    *next = '\0';
    p = next + 1;
  }
  return count;
}

// "高温 13℃" / "低温 -2℃" -> 13 / -2
int parseTemp(const char* s) {
  if (s == nullptr) return 0;
  while (*s != '\0' && *s != '-' && (*s < '0' || *s > '9')) ++s;
  return std::atoi(s);
}

// Collapse the administrative and ethnic descriptors that amap and the vendor
// city_code.txt disagree on, so "青海省"/"青海", "广西壮族自治区"/"广西" and
// "海南藏族自治州"/"海南" all compare equal. Applied repeatedly: the ethnic
// names sit inside the administrative suffix.
void trimAdminSuffix(char* s) {
  static const char* const kSuffixes[] = {
      "特别行政区", "自治区", "自治州", "自治县", "自治旗", "地区",   "维吾尔",   "柯尔克孜", "哈萨克",   "蒙古族",
      "藏族",       "壮族",   "回族",   "满族",   "彝族",   "苗族",   "侗族",     "布依族",   "傣族",     "景颇族",
      "白族",       "哈尼族", "傈僳族", "拉祜族", "佤族",   "纳西族", "瑶族",     "畲族",     "黎族",     "土家族",
      "朝鲜族",     "羌族",   "锡伯族", "裕固族", "土族",   "撒拉族", "达斡尔族", "鄂温克族", "鄂伦春族", "赫哲族",
      "门巴族",     "珞巴族", "基诺族", "布朗族", "阿昌族", "普米族", "怒族",     "德昂族",   "独龙族",   "仡佬族",
      "仫佬族",     "毛南族", "京族",   "水族",   "省",     "市",     "区",       "县",       "盟",       "旗"};
  bool changed = true;
  while (changed) {
    changed = false;
    const size_t len = std::strlen(s);
    for (const char* suffix : kSuffixes) {
      const size_t sl = std::strlen(suffix);
      if (len > sl && std::strcmp(s + len - sl, suffix) == 0) {
        s[len - sl] = '\0';
        changed = true;
        break;
      }
    }
  }
}

// Equal, or one name is a prefix of the other ("阿拉善" vs "阿拉善左旗").
// Only used for city names, where the vendor table may list a seat while amap
// reports the enclosing league/prefecture.
bool sameOrPrefix(const char* a, const char* b) {
  if (std::strcmp(a, b) == 0) return true;
  const size_t la = std::strlen(a);
  const size_t lb = std::strlen(b);
  if (la < 2 || lb < 2) return false;
  const size_t n = la < lb ? la : lb;
  return std::strncmp(a, b, n) == 0;
}

// --- amap IP location -------------------------------------------------------

class AmapParser {
 public:
  AmapParser()
      : parser(JsonCallbacks{this, sOnKey, sOnString, sOnNumber, sOnBool, sOnNull, sOnObjectStart, sOnObjectEnd,
                             sOnArrayStart, sOnArrayEnd}) {}

  void feed(const char* data, size_t len) { parser.feed(data, len); }
  bool failed() const { return parser.hasError(); }

  char province[24] = {};
  char city[24] = {};
  char adcode[16] = {};

 private:
  enum class Key : uint8_t { None, Province, City, Adcode };

  static void sOnKey(void* ctx, const char* key, size_t len) {
    auto* self = static_cast<AmapParser*>(ctx);
    self->key = Key::None;
    if (self->depth != 1) return;
    if (len == 8 && std::memcmp(key, "province", 8) == 0)
      self->key = Key::Province;
    else if (len == 4 && std::memcmp(key, "city", 4) == 0)
      self->key = Key::City;
    else if (len == 6 && std::memcmp(key, "adcode", 6) == 0)
      self->key = Key::Adcode;
  }

  static void sOnString(void* ctx, const char* value, size_t len) {
    auto* self = static_cast<AmapParser*>(ctx);
    switch (self->key) {
      case Key::Province:
        copyField(self->province, sizeof(self->province), value, len);
        break;
      case Key::City:
        copyField(self->city, sizeof(self->city), value, len);
        break;
      case Key::Adcode:
        copyField(self->adcode, sizeof(self->adcode), value, len);
        break;
      case Key::None:
        break;
    }
    self->key = Key::None;
  }

  static void sOnNumber(void* ctx, const char*, size_t) { static_cast<AmapParser*>(ctx)->key = Key::None; }
  static void sOnBool(void* ctx, bool) { static_cast<AmapParser*>(ctx)->key = Key::None; }
  static void sOnNull(void* ctx) { static_cast<AmapParser*>(ctx)->key = Key::None; }

  static void sOnObjectStart(void* ctx) { ++static_cast<AmapParser*>(ctx)->depth; }
  static void sOnObjectEnd(void* ctx) {
    auto* self = static_cast<AmapParser*>(ctx);
    if (self->depth > 0) --self->depth;
    self->key = Key::None;
  }
  static void sOnArrayStart(void* ctx) { ++static_cast<AmapParser*>(ctx)->depth; }
  static void sOnArrayEnd(void* ctx) {
    auto* self = static_cast<AmapParser*>(ctx);
    if (self->depth > 0) --self->depth;
    self->key = Key::None;
  }

  StreamingJsonParser parser;
  uint8_t depth = 0;
  Key key = Key::None;
};

// --- sojson forecast --------------------------------------------------------

class WeatherJsonParser {
 public:
  explicit WeatherJsonParser(WeatherNow& out)
      : parser(JsonCallbacks{this, sOnKey, sOnString, sOnNumber, sOnBool, sOnNull, sOnObjectStart, sOnObjectEnd,
                             sOnArrayStart, sOnArrayEnd}),
        out(out) {}

  void feed(const char* data, size_t len) { parser.feed(data, len); }
  bool failed() const { return parser.hasError(); }

  void finalize() { out.valid = out.city[0] != '\0' && out.dayCount > 0; }

 private:
  enum class Section : uint8_t { Root, City, Data, Forecast, Day };
  enum class Key : uint8_t {
    None,
    CityInfo,
    Data,
    Forecast,
    City,
    Parent,
    UpdateTime,
    Wendu,
    Shidu,
    Quality,
    Ymd,
    Type,
    High,
    Low,
    Fx,
    Fl,
    Sunrise,
    Sunset,
  };

  void beginDay() {
    day = WeatherDay{};
    key = Key::None;
  }

  void commitDay() {
    if (dayIndex < WeatherNow::kMaxDays) {
      out.days[dayIndex] = day;
      ++dayIndex;
      out.dayCount = dayIndex;
    }
  }

  static void sOnKey(void* ctx, const char* k, size_t len) {
    auto* self = static_cast<WeatherJsonParser*>(ctx);
    self->key = Key::None;

    if (self->section == Section::Root && self->depth == 1) {
      if (len == 8 && std::memcmp(k, "cityInfo", 8) == 0)
        self->key = Key::CityInfo;
      else if (len == 4 && std::memcmp(k, "data", 4) == 0)
        self->key = Key::Data;
      return;
    }
    if (self->section == Section::City && self->depth == self->cityDepth) {
      if (len == 4 && std::memcmp(k, "city", 4) == 0)
        self->key = Key::City;
      else if (len == 6 && std::memcmp(k, "parent", 6) == 0)
        self->key = Key::Parent;
      else if (len == 10 && std::memcmp(k, "updateTime", 10) == 0)
        self->key = Key::UpdateTime;
      return;
    }
    if (self->section == Section::Data && self->depth == self->dataDepth) {
      if (len == 8 && std::memcmp(k, "forecast", 8) == 0)
        self->key = Key::Forecast;
      else if (len == 5 && std::memcmp(k, "wendu", 5) == 0)
        self->key = Key::Wendu;
      else if (len == 5 && std::memcmp(k, "shidu", 5) == 0)
        self->key = Key::Shidu;
      else if (len == 7 && std::memcmp(k, "quality", 7) == 0)
        self->key = Key::Quality;
      return;
    }
    if (self->section == Section::Day && self->depth == self->dayDepth) {
      if (len == 3 && std::memcmp(k, "ymd", 3) == 0)
        self->key = Key::Ymd;
      else if (len == 4 && std::memcmp(k, "type", 4) == 0)
        self->key = Key::Type;
      else if (len == 4 && std::memcmp(k, "high", 4) == 0)
        self->key = Key::High;
      else if (len == 3 && std::memcmp(k, "low", 3) == 0)
        self->key = Key::Low;
      else if (len == 2 && std::memcmp(k, "fx", 2) == 0)
        self->key = Key::Fx;
      else if (len == 2 && std::memcmp(k, "fl", 2) == 0)
        self->key = Key::Fl;
      else if (len == 7 && std::memcmp(k, "sunrise", 7) == 0)
        self->key = Key::Sunrise;
      else if (len == 6 && std::memcmp(k, "sunset", 6) == 0)
        self->key = Key::Sunset;
    }
  }

  static void sOnString(void* ctx, const char* value, size_t len) {
    auto* self = static_cast<WeatherJsonParser*>(ctx);
    WeatherDay& d = self->day;
    switch (self->key) {
      case Key::City:
        copyField(self->out.city, sizeof(self->out.city), value, len);
        break;
      case Key::Parent:
        copyField(self->out.province, sizeof(self->out.province), value, len);
        break;
      case Key::UpdateTime:
        copyField(self->out.updateTime, sizeof(self->out.updateTime), value, len);
        break;
      case Key::Wendu:
        copyField(self->out.wendu, sizeof(self->out.wendu), value, len);
        break;
      case Key::Shidu:
        copyField(self->out.shidu, sizeof(self->out.shidu), value, len);
        break;
      case Key::Quality:
        copyField(self->out.quality, sizeof(self->out.quality), value, len);
        break;
      case Key::Ymd:
        copyField(d.ymd, sizeof(d.ymd), value, len);
        break;
      case Key::Type:
        copyField(d.type, sizeof(d.type), value, len);
        break;
      case Key::High:
        d.high = static_cast<int8_t>(parseTemp(value));
        break;
      case Key::Low:
        d.low = static_cast<int8_t>(parseTemp(value));
        break;
      case Key::Fx:
        copyField(d.fx, sizeof(d.fx), value, len);
        break;
      case Key::Fl:
        copyField(d.fl, sizeof(d.fl), value, len);
        break;
      case Key::Sunrise:
        copyField(d.sunrise, sizeof(d.sunrise), value, len);
        break;
      case Key::Sunset:
        copyField(d.sunset, sizeof(d.sunset), value, len);
        break;
      default:
        break;
    }
    self->key = Key::None;
  }

  static void sOnNumber(void* ctx, const char* value, size_t len) {
    // aqi/pm25 arrive as numbers sometimes; they are not rendered.
    static_cast<WeatherJsonParser*>(ctx)->key = Key::None;
    (void)value;
    (void)len;
  }
  static void sOnBool(void* ctx, bool) { static_cast<WeatherJsonParser*>(ctx)->key = Key::None; }
  static void sOnNull(void* ctx) { static_cast<WeatherJsonParser*>(ctx)->key = Key::None; }

  static void sOnObjectStart(void* ctx) {
    auto* self = static_cast<WeatherJsonParser*>(ctx);

    if (self->section == Section::Root) {
      ++self->depth;
      if (self->depth == 2 && self->key == Key::CityInfo) {
        self->section = Section::City;
        self->cityDepth = 2;
      } else if (self->depth == 2 && self->key == Key::Data) {
        self->section = Section::Data;
        self->dataDepth = 2;
      }
    } else if (self->section == Section::Forecast) {
      ++self->depth;
      if (self->depth == self->forecastDepth + 1) {
        self->section = Section::Day;
        self->dayDepth = self->depth;
        self->beginDay();
      }
    } else {
      ++self->depth;
    }
    self->key = Key::None;
  }

  static void sOnObjectEnd(void* ctx) {
    auto* self = static_cast<WeatherJsonParser*>(ctx);

    if (self->section == Section::Day && self->depth == self->dayDepth) {
      self->commitDay();
      self->section = Section::Forecast;
    } else if (self->section == Section::City && self->depth == self->cityDepth) {
      self->section = Section::Root;
    } else if (self->section == Section::Data && self->depth == self->dataDepth) {
      self->section = Section::Root;
    }
    if (self->depth > 0) --self->depth;
    self->key = Key::None;
  }

  static void sOnArrayStart(void* ctx) {
    auto* self = static_cast<WeatherJsonParser*>(ctx);

    if (self->section == Section::Data && self->depth == self->dataDepth && self->key == Key::Forecast) {
      ++self->depth;
      self->section = Section::Forecast;
      self->forecastDepth = self->depth;
    } else {
      ++self->depth;
    }
    self->key = Key::None;
  }

  static void sOnArrayEnd(void* ctx) {
    auto* self = static_cast<WeatherJsonParser*>(ctx);

    if (self->section == Section::Forecast && self->depth == self->forecastDepth) {
      self->section = Section::Data;
    }
    if (self->depth > 0) --self->depth;
    self->key = Key::None;
  }

  StreamingJsonParser parser;
  WeatherNow& out;
  Section section = Section::Root;
  Key key = Key::None;
  uint8_t depth = 0;
  uint8_t cityDepth = 0;
  uint8_t dataDepth = 0;
  uint8_t forecastDepth = 0;
  uint8_t dayDepth = 0;
  uint8_t dayIndex = 0;
  WeatherDay day;
};

// --- city_code.txt lookup ---------------------------------------------------

bool lookupCityCode(const char* province, const char* city, const char* adcode, char* out, size_t capacity) {
  HalFile file;
  if (!Storage.openFileForRead("WTHR", kCityFile, file)) return false;

  char line[kLineMax];
  char byAdcode[16] = {};
  char byCityPrefix[16] = {};
  char byCityOnly[16] = {};
  while (readLine(file, line, sizeof(line))) {
    char* fields[5];
    const int count = splitFields(line, ',', fields, 5);
    if (count < 3) continue;

    char* fileProv = trimInPlace(fields[0]);
    char* fileCity = trimInPlace(fields[1]);
    // 3 columns: 省,市,code — 4 columns: 省,市,adcode,code.
    char* fileAdcode = (count >= 4) ? trimInPlace(fields[2]) : nullptr;
    char* fileCode = trimInPlace(fields[count >= 4 ? 3 : 2]);
    // The vendor table keeps suffixes ("青海省", "广西壮族自治区") while amap's
    // names were already collapsed, so normalise the row before comparing.
    trimAdminSuffix(fileProv);
    trimAdminSuffix(fileCity);

    if (std::strcmp(fileProv, province) == 0 && std::strcmp(fileCity, city) == 0) {
      copyField(out, capacity, fileCode, std::strlen(fileCode));
      return true;
    }
    if (adcode != nullptr && adcode[0] != '\0' && fileAdcode != nullptr && byAdcode[0] == '\0' &&
        std::strcmp(fileAdcode, adcode) == 0) {
      copyField(byAdcode, sizeof(byAdcode), fileCode, std::strlen(fileCode));
    }
    if (byCityPrefix[0] == '\0' && std::strcmp(fileProv, province) == 0 && sameOrPrefix(fileCity, city)) {
      copyField(byCityPrefix, sizeof(byCityPrefix), fileCode, std::strlen(fileCode));
    }
    if (byCityOnly[0] == '\0' && std::strcmp(fileCity, city) == 0) {
      copyField(byCityOnly, sizeof(byCityOnly), fileCode, std::strlen(fileCode));
    }
  }

  if (byAdcode[0] != '\0') {
    copyField(out, capacity, byAdcode, std::strlen(byAdcode));
    return true;
  }
  if (byCityPrefix[0] != '\0') {
    copyField(out, capacity, byCityPrefix, std::strlen(byCityPrefix));
    return true;
  }
  if (byCityOnly[0] != '\0') {
    copyField(out, capacity, byCityOnly, std::strlen(byCityOnly));
    return true;
  }
  return false;
}

}  // namespace

namespace weather {

bool loadCached(WeatherNow& out) {
  out = WeatherNow{};

  char buffer[1280];
  if (Storage.readFileToBuffer(kCachePath, buffer, sizeof(buffer)) == 0) return false;

  bool sawHeader = false;
  uint8_t dayCount = 0;
  char* cursor = buffer;
  while (*cursor != '\0') {
    char* eol = std::strchr(cursor, '\n');
    if (eol != nullptr) *eol = '\0';

    if (!sawHeader) {
      if (std::strcmp(trimInPlace(cursor), "V1") == 0) sawHeader = true;
    } else if (cursor[0] == 'F' && (cursor[1] == '|' || cursor[1] == '\0')) {
      char* fields[9];
      const int count = splitFields(cursor, '|', fields, 9);
      if (count >= 3) {
        out.fetchedAt = static_cast<uint32_t>(std::strtoul(fields[1], nullptr, 10));
        copyField(out.province, sizeof(out.province), fields[2], std::strlen(fields[2]));
        if (count >= 4) copyField(out.city, sizeof(out.city), fields[3], std::strlen(fields[3]));
        if (count >= 5) copyField(out.wendu, sizeof(out.wendu), fields[4], std::strlen(fields[4]));
        if (count >= 6) copyField(out.shidu, sizeof(out.shidu), fields[5], std::strlen(fields[5]));
        if (count >= 7) copyField(out.quality, sizeof(out.quality), fields[6], std::strlen(fields[6]));
        if (count >= 8) copyField(out.updateTime, sizeof(out.updateTime), fields[7], std::strlen(fields[7]));
      }
    } else if (cursor[0] == 'D' && (cursor[1] == '|' || cursor[1] == '\0') && dayCount < WeatherNow::kMaxDays) {
      char* fields[10];
      const int count = splitFields(cursor, '|', fields, 10);
      if (count >= 5) {
        WeatherDay& day = out.days[dayCount];
        copyField(day.ymd, sizeof(day.ymd), fields[1], std::strlen(fields[1]));
        copyField(day.type, sizeof(day.type), fields[2], std::strlen(fields[2]));
        day.high = static_cast<int8_t>(std::atoi(fields[3]));
        day.low = static_cast<int8_t>(std::atoi(fields[4]));
        if (count >= 6) copyField(day.fx, sizeof(day.fx), fields[5], std::strlen(fields[5]));
        if (count >= 7) copyField(day.fl, sizeof(day.fl), fields[6], std::strlen(fields[6]));
        if (count >= 8) copyField(day.sunrise, sizeof(day.sunrise), fields[7], std::strlen(fields[7]));
        if (count >= 9) copyField(day.sunset, sizeof(day.sunset), fields[8], std::strlen(fields[8]));
        ++dayCount;
      }
    }

    if (eol == nullptr) break;
    cursor = eol + 1;
  }

  out.dayCount = dayCount;
  out.valid = sawHeader && out.city[0] != '\0' && dayCount > 0;
  return out.valid;
}

void saveCached(const WeatherNow& in) {
  if (!in.valid) return;
  Storage.mkdir(kCacheDir);

  HalFile file;
  if (!Storage.openFileForWrite("WTHR", kCacheTmpPath, file)) return;

  char line[kLineMax];
  int written =
      std::snprintf(line, sizeof(line), "V1\nF|%lu|%s|%s|%s|%s|%s|%s\n", static_cast<unsigned long>(in.fetchedAt),
                    in.province, in.city, in.wendu, in.shidu, in.quality, in.updateTime);
  if (written > 0) file.write(line, static_cast<size_t>(written));

  for (uint8_t i = 0; i < in.dayCount; ++i) {
    const WeatherDay& day = in.days[i];
    written =
        std::snprintf(line, sizeof(line), "D|%s|%s|%d|%d|%s|%s|%s|%s\n", day.ymd, day.type, static_cast<int>(day.high),
                      static_cast<int>(day.low), day.fx, day.fl, day.sunrise, day.sunset);
    if (written > 0) file.write(line, static_cast<size_t>(written));
  }
  file.flush();
  // HalFile's destructor closes; replaceFile() needs the handle released.
  file = HalFile();
  Storage.replaceFile(kCacheTmpPath, kCachePath);
}

Status refresh(WeatherNow& out) {
  if (WiFi.status() != WL_CONNECTED) return Status::Network;

  // 1) amap IP location. The SAX parser's 2 KiB token buffer is heap-allocated
  // so the loop-task stack (8 KiB) keeps its headroom.
  auto amap = makeUniqueNoThrow<AmapParser>();
  if (!amap) {
    LOG_ERR("WTHR", "OOM: amap parser");
    return Status::Parse;
  }
  const bool amapOk = HttpDownloader::fetchUrl(kAmapUrl, [&amap](const uint8_t* data, size_t len) {
    amap->feed(reinterpret_cast<const char*>(data), len);
    return true;
  });
  if (!amapOk || amap->failed() || amap->city[0] == '\0') {
    LOG_ERR("WTHR", "IP location failed (http=%d, city='%s')", amapOk ? 1 : 0, amap->city);
    return Status::Locate;
  }

  char province[sizeof(AmapParser::province)];
  char city[sizeof(AmapParser::city)];
  copyField(province, sizeof(province), amap->province, std::strlen(amap->province));
  copyField(city, sizeof(city), amap->city, std::strlen(amap->city));
  trimAdminSuffix(province);
  trimAdminSuffix(city);

  char cityCode[16] = {};
  if (!Storage.exists(kCityFile)) {
    LOG_ERR("WTHR", "%s missing on SD", kCityFile);
    return Status::NoCardFile;
  }
  if (!lookupCityCode(province, city, amap->adcode, cityCode, sizeof(cityCode)) || cityCode[0] == '\0') {
    LOG_ERR("WTHR", "No city code for '%s/%s' (adcode '%s')", province, city, amap->adcode);
    return Status::CityUnmatched;
  }

  // 2) sojson forecast.
  WeatherNow parsed;
  auto wx = makeUniqueNoThrow<WeatherJsonParser>(parsed);
  if (!wx) {
    LOG_ERR("WTHR", "OOM: weather parser");
    return Status::Parse;
  }

  std::string url = kSojsonPrefix;
  url += cityCode;
  // A rejected or rewritten answer (bad UA, rate limit, gzip) still frames as a
  // complete body, so the failure surfaces as "days=0" with no parser error.
  // Keep the byte count plus a printable head so the serial log names the cause.
  size_t forecastBytes = 0;
  char head[97] = {};
  size_t headLen = 0;
  const bool fetchOk =
      HttpDownloader::fetchUrl(url, [&wx, &forecastBytes, &head, &headLen](const uint8_t* data, size_t len) {
        forecastBytes += len;
        if (headLen < sizeof(head) - 1) {
          const size_t take = std::min(len, sizeof(head) - 1 - headLen);
          std::memcpy(head + headLen, data, take);
          headLen += take;
          head[headLen] = '\0';
        }
        wx->feed(reinterpret_cast<const char*>(data), len);
        return true;
      });
  for (size_t i = 0; i < headLen; ++i) {
    const unsigned char c = static_cast<unsigned char>(head[i]);
    if (c < 0x20 || c > 0x7e) head[i] = '.';
  }
  if (!fetchOk || wx->failed()) {
    LOG_ERR("WTHR", "Forecast fetch failed (http=%d rx=%u head=%s)", fetchOk ? 1 : 0,
            static_cast<unsigned>(forecastBytes), head);
    return Status::Network;
  }
  wx->finalize();
  if (!parsed.valid) {
    LOG_ERR("WTHR", "Forecast payload unusable (days=%u rx=%u head=%s)", static_cast<unsigned>(parsed.dayCount),
            static_cast<unsigned>(forecastBytes), head);
    return Status::Parse;
  }

  parsed.fetchedAt = TimeUtils::getCurrentValidTimestamp();
  out = parsed;
  saveCached(out);
  LOG_DBG("WTHR", "%s/%s %s days=%u", out.province, out.city, out.wendu, static_cast<unsigned>(out.dayCount));
  return Status::Ok;
}

bool isStale(const WeatherNow& now, uint32_t nowSec) {
  if (!now.valid) return true;
  if (nowSec == 0 || now.fetchedAt == 0) return false;  // clock unknown: don't spin
  if (nowSec < now.fetchedAt) return true;
  return nowSec - now.fetchedAt >= kRefreshIntervalSec;
}

const char* statusText(Status status) {
  switch (status) {
    case Status::Ok:
      return "ok";
    case Status::NoCache:
      return "no-cache";
    case Status::Network:
      return "network";
    case Status::Locate:
      return "locate-failed";
    case Status::CityUnmatched:
      return "city-unmatched";
    case Status::NoCardFile:
      return "city_code.txt missing";
    case Status::Parse:
      return "parse";
  }
  return "unknown";
}

}  // namespace weather
