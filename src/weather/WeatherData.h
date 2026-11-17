#pragma once

#include <cstdint>

// One forecast row from the sojson weather API. Every string is fixed-size so a
// WeatherNow can live on the stack and be cached without any allocation.
struct WeatherDay {
  char ymd[11] = {};     // "2026-11-15"
  char type[20] = {};    // "晴"
  char fx[16] = {};      // "西北风"
  char fl[8] = {};       // "<3级"
  char sunrise[8] = {};  // "06:45"
  char sunset[8] = {};   // "17:12"
  int8_t high = 0;
  int8_t low = 0;
};

// Full weather snapshot shown by the standby weather face.
struct WeatherNow {
  static constexpr uint8_t kMaxDays = 4;

  bool valid = false;
  uint32_t fetchedAt = 0;  // local epoch seconds of the successful fetch
  char province[20] = {};
  char city[20] = {};
  char wendu[8] = {};       // current temperature, unit-less ("12")
  char shidu[12] = {};      // "53%"
  char quality[12] = {};    // "良"
  char updateTime[8] = {};  // provider update time "10:20"
  uint8_t dayCount = 0;
  WeatherDay days[kMaxDays] = {};
};

namespace weather {

enum class Status : uint8_t {
  Ok = 0,
  NoCache,        // nothing usable on SD
  Network,        // no WiFi link / HTTP failure
  Locate,         // amap IP location failed
  CityUnmatched,  // /city_code.txt has no row for the detected city
  NoCardFile,     // /city_code.txt missing
  Parse,          // response body could not be parsed
};

// The provider refreshes four times a day (04/09/14/20) and its CDN caches for
// an hour, so polling faster than this only wastes power.
constexpr uint32_t kRefreshIntervalSec = 3600;

// Text cache helpers ( /.crosspoint/weather.txt ).
bool loadCached(WeatherNow& out);
void saveCached(const WeatherNow& in);

// Fetch amap IP location + sojson forecast. Requires an active WiFi link; the
// caller owns connect/teardown. On success the snapshot is also cached to SD.
Status refresh(WeatherNow& out);

// True when the snapshot is older than kRefreshIntervalSec. With an unknown
// wall clock we report "fresh" so a standby face cannot spin on refresh.
bool isStale(const WeatherNow& now, uint32_t nowSec);

// Log-only English description.
const char* statusText(Status status);

}  // namespace weather
