#include "WeatherFace.h"

#include <Arduino.h>
#include <Bitmap.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "activities/RenderLock.h"
#include "fontIds.h"
#include "util/TimeUtils.h"

namespace {

constexpr char kIconDir[] = "/Weather_img/Weather_img";
constexpr char kIconIndex[] = "/Weather_img/Weather_img/Weather.txt";
constexpr uint32_t kRetryBackoffMs = 5u * 60u * 1000u;

void drawCentered(const GfxRenderer& renderer, int fontId, const Rect& viewport, int y, const char* text) {
  if (text == nullptr || text[0] == '\0') return;
  const int w = renderer.getTextWidth(fontId, text);
  renderer.drawText(fontId, viewport.x + (viewport.width - w) / 2, y, text, /*black=*/true);
}

void drawRight(const GfxRenderer& renderer, int fontId, int right, int y, const char* text) {
  if (text == nullptr || text[0] == '\0') return;
  const int w = renderer.getTextWidth(fontId, text);
  renderer.drawText(fontId, right - w, y, text, /*black=*/true);
}

// "2026-11-15" -> "11/15"
void formatMonthDay(const char* ymd, char* out, size_t capacity) {
  out[0] = '\0';
  if (ymd == nullptr || std::strlen(ymd) < 10) return;
  std::snprintf(out, capacity, "%d/%d", std::atoi(ymd + 5), std::atoi(ymd + 8));
}

void formatTempRange(const WeatherDay& day, char* out, size_t capacity) {
  std::snprintf(out, capacity, tr(STR_WEATHER_TEMP_RANGE_FMT), static_cast<int>(day.high), static_cast<int>(day.low));
}

char* trimLine(char* s) {
  size_t len = std::strlen(s);
  while (len > 0 && (s[len - 1] == '\r' || s[len - 1] == '\n' || s[len - 1] == ' ')) s[--len] = '\0';
  while (*s == ' ' || *s == '\t') ++s;
  return s;
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

}  // namespace

void WeatherFace::onEnter() {
  load();
  if (weather::isStale(data_, TimeUtils::getCurrentValidTimestamp())) lastAttemptMs_ = 0;
}

void WeatherFace::onExit() {}

void WeatherFace::load() {
  RenderLock lock;
  weather::loadCached(data_);
  resolveIcons();
  LOG_DBG("WTHR", "cache %s (%s/%s)", data_.valid ? "hit" : "miss", data_.province, data_.city);
}

void WeatherFace::resolveIcons() {
  for (auto& path : iconPath_) path[0] = '\0';
  if (!data_.valid) return;

  HalFile file;
  if (!Storage.openFileForRead("WTHR", kIconIndex, file)) return;

  char line[96];
  while (readLine(file, line, sizeof(line))) {
    char* comma = std::strchr(line, ',');
    if (comma == nullptr) continue;
    *comma = '\0';
    const char* label = trimLine(line);
    char* file = trimLine(comma + 1);
    if (label[0] == '\0' || file[0] == '\0') continue;

    for (uint8_t i = 0; i < data_.dayCount && i < WeatherNow::kMaxDays; ++i) {
      if (iconPath_[i][0] != '\0' || data_.days[i].type[0] == '\0') continue;
      if (std::strcmp(label, data_.days[i].type) != 0) continue;
      std::snprintf(iconPath_[i], sizeof(iconPath_[i]), "%s/%s.bmp", kIconDir, file);
    }
  }
}

void WeatherFace::drawIcon(GfxRenderer& renderer, uint8_t dayIndex, int x, int y, int size) const {
  if (dayIndex >= WeatherNow::kMaxDays) return;
  const char* path = iconPath_[dayIndex];
  if (path[0] == '\0') return;

  HalFile file;
  if (!Storage.openFileForRead("WTHR", path, file)) return;
  Bitmap bitmap(file);
  if (bitmap.parseHeaders() != BmpReaderError::Ok) return;
  renderer.drawBitmap(bitmap, x, y, size, size);
}

bool WeatherFace::wantsNetworkSync() {
  if (lastAttemptMs_ != 0 && millis() - lastAttemptMs_ < kRetryBackoffMs) return false;
  if (!weather::isStale(data_, TimeUtils::getCurrentValidTimestamp())) return false;
  // Stamp the attempt here, not in performNetworkSync(): when no saved WiFi
  // credentials exist StandbyActivity never starts a session, and without this
  // stamp the face would re-request every loop tick.
  lastAttemptMs_ = millis();
  return true;
}

bool WeatherFace::performNetworkSync() {
  lastAttemptMs_ = millis();

  WeatherNow next;
  const weather::Status status = weather::refresh(next);
  if (status == weather::Status::Ok) {
    data_ = next;
    error_ = Error::None;
    {
      RenderLock lock;
      resolveIcons();
    }
    return true;
  }

  LOG_DBG("WTHR", "refresh failed: %s", weather::statusText(status));
  switch (status) {
    case weather::Status::Locate:
      error_ = Error::Locate;
      break;
    case weather::Status::CityUnmatched:
      error_ = Error::City;
      break;
    case weather::Status::NoCardFile:
      error_ = Error::CardFile;
      break;
    case weather::Status::Parse:
      error_ = Error::Parse;
      break;
    default:
      error_ = Error::Network;
      break;
  }
  // Only repaint when we had nothing on screen; otherwise keep the stale cache.
  return !data_.valid;
}

StandbyFace::TickResult WeatherFace::tick() { return TickResult::None; }

void WeatherFace::render(GfxRenderer& renderer, const Rect& viewport) {
  if (viewport.width <= 0 || viewport.height <= 0) return;

  if (!data_.valid) {
    renderUnavailable(renderer, viewport);
    return;
  }

  if (viewport.height > viewport.width) {
    renderPortrait(renderer, viewport);
  } else {
    renderLandscape(renderer, viewport);
  }
}

void WeatherFace::renderUnavailable(GfxRenderer& renderer, const Rect& viewport) {
  const char* detail = nullptr;
  switch (error_) {
    case Error::Locate:
      detail = tr(STR_WEATHER_ERROR_LOCATE);
      break;
    case Error::City:
      detail = tr(STR_WEATHER_ERROR_CITY);
      break;
    case Error::CardFile:
      detail = tr(STR_WEATHER_ERROR_FILE);
      break;
    case Error::Network:
    case Error::Parse:
      detail = tr(STR_WEATHER_ERROR_NETWORK);
      break;
    case Error::None:
      break;
  }

  const int midY = viewport.y + viewport.height / 2;
  drawCentered(renderer, NOTOSANS_12_FONT_ID, viewport, midY - 30, tr(STR_WEATHER_NO_DATA));
  drawCentered(renderer, SMALL_FONT_ID, viewport, midY + 2, tr(STR_WEATHER_UPDATING));
  if (detail != nullptr) drawCentered(renderer, SMALL_FONT_ID, viewport, midY + 26, detail);
}

void WeatherFace::renderPortrait(GfxRenderer& renderer, const Rect& viewport) {
  const int vx = viewport.x;
  const int vw = viewport.width;
  const int top = viewport.y + 42;

  char line[80];
  const char* label = data_.city[0] != '\0' ? data_.city : data_.province;
  drawCentered(renderer, NOTOSANS_12_FONT_ID, viewport, top, label);

  // The update time lives in the bottom-right corner: centred under the city it
  // collided with the label on this panel. It also gets its own footer band so
  // the forecast rows stop above it instead of drawing through it.
  char updated[80] = {};
  const bool hasUpdated = data_.updateTime[0] != '\0';
  if (hasUpdated) std::snprintf(updated, sizeof(updated), tr(STR_WEATHER_UPDATED_FMT), data_.updateTime);
  const int footerY = viewport.y + viewport.height - renderer.getLineHeight(SMALL_FONT_ID) - 6;

  if (data_.wendu[0] != '\0') {
    std::snprintf(line, sizeof(line), "%s°C", data_.wendu);
    drawCentered(renderer, NOTOSANS_18_FONT_ID, viewport, top + 46, line);
  }

  int y = top + 86;
  if (data_.shidu[0] != '\0' || data_.quality[0] != '\0') {
    std::snprintf(line, sizeof(line), "%s %s   %s %s", tr(STR_WEATHER_HUMIDITY_LABEL), data_.shidu,
                  tr(STR_WEATHER_QUALITY_LABEL), data_.quality);
    drawCentered(renderer, SMALL_FONT_ID, viewport, y, line);
    y += 22;
  }

  const WeatherDay& today = data_.days[0];
  if (today.fx[0] != '\0' || today.fl[0] != '\0') {
    std::snprintf(line, sizeof(line), "%s %s", today.fx, today.fl);
    drawCentered(renderer, SMALL_FONT_ID, viewport, y, line);
    y += 22;
  }
  if (today.sunrise[0] != '\0' || today.sunset[0] != '\0') {
    std::snprintf(line, sizeof(line), "%s / %s", today.sunrise, today.sunset);
    drawCentered(renderer, SMALL_FONT_ID, viewport, y, line);
  }

  const int dividerY = top + 164;
  renderer.drawLine(vx + 32, dividerY, vx + vw - 32, dividerY, true);

  const int rowsTop = dividerY + 18;
  const int rowsBottom = hasUpdated ? footerY - 8 : viewport.y + viewport.height - 24;
  const int rows = data_.dayCount > 0 ? static_cast<int>(data_.dayCount) : 1;
  const int rowH = std::max(48, (rowsBottom - rowsTop) / rows);
  const int iconBox = std::min(rowH - 16, 96);
  const int textX = vx + 20 + 96 + 16;
  const int typeX = vx + vw * 52 / 100;
  const int rangeRight = vx + vw - 24;

  for (int i = 0; i < rows; ++i) {
    const WeatherDay& day = data_.days[i];
    const int rowY = rowsTop + i * rowH;
    const int textY = rowY + (rowH - renderer.getFontAscenderSize(NOTOSANS_12_FONT_ID)) / 2;

    drawIcon(renderer, static_cast<uint8_t>(i), vx + 20, rowY + (rowH - iconBox) / 2, iconBox);

    char date[16];
    if (i == 0) {
      std::snprintf(date, sizeof(date), "%s", tr(STR_WEATHER_TODAY));
    } else {
      formatMonthDay(day.ymd, date, sizeof(date));
    }
    renderer.drawText(NOTOSANS_12_FONT_ID, textX, textY, date, true);
    renderer.drawText(NOTOSANS_12_FONT_ID, typeX, textY, day.type, true);

    char range[24];
    formatTempRange(day, range, sizeof(range));
    drawRight(renderer, NOTOSANS_12_FONT_ID, rangeRight, textY, range);

    if (i + 1 < rows) {
      const int sepY = rowY + rowH - 1;
      renderer.drawLine(vx + 24, sepY, vx + vw - 24, sepY, true);
    }
  }

  if (hasUpdated) drawRight(renderer, SMALL_FONT_ID, vx + vw - 24, footerY, updated);
}

void WeatherFace::renderLandscape(GfxRenderer& renderer, const Rect& viewport) {
  const int vx = viewport.x;
  const int vw = viewport.width;
  const int top = viewport.y + 36;

  char line[80];
  const char* label = data_.city[0] != '\0' ? data_.city : data_.province;
  renderer.drawText(NOTOSANS_12_FONT_ID, vx + 24, top, label, true);

  if (data_.updateTime[0] != '\0') {
    std::snprintf(line, sizeof(line), tr(STR_WEATHER_UPDATED_FMT), data_.updateTime);
    drawRight(renderer, SMALL_FONT_ID, vx + vw - 24, top + 2, line);
  }

  int y = top + 26;
  if (data_.wendu[0] != '\0') {
    std::snprintf(line, sizeof(line), "%s°C", data_.wendu);
    renderer.drawText(NOTOSANS_18_FONT_ID, vx + 24, y, line, true);
    y += 30;
  }
  if (data_.shidu[0] != '\0' || data_.quality[0] != '\0') {
    std::snprintf(line, sizeof(line), "%s %s   %s %s", tr(STR_WEATHER_HUMIDITY_LABEL), data_.shidu,
                  tr(STR_WEATHER_QUALITY_LABEL), data_.quality);
    renderer.drawText(SMALL_FONT_ID, vx + 24, y, line, true);
    y += 20;
  }
  const WeatherDay& today = data_.days[0];
  if (today.fx[0] != '\0' || today.fl[0] != '\0') {
    std::snprintf(line, sizeof(line), "%s %s", today.fx, today.fl);
    renderer.drawText(SMALL_FONT_ID, vx + 24, y, line, true);
  }

  const int dividerY = y + 16;
  renderer.drawLine(vx + 16, dividerY, vx + vw - 16, dividerY, true);

  const int colW = vw / 4;
  const int colTop = dividerY + 10;
  const int iconBox = 80;

  for (int i = 0; i < static_cast<int>(WeatherNow::kMaxDays); ++i) {
    if (i >= data_.dayCount) break;
    const WeatherDay& day = data_.days[i];
    const int cx = vx + i * colW;

    char date[16];
    if (i == 0) {
      std::snprintf(date, sizeof(date), "%s", tr(STR_WEATHER_TODAY));
    } else {
      formatMonthDay(day.ymd, date, sizeof(date));
    }
    drawCentered(renderer, NOTOSANS_12_FONT_ID, Rect{cx, viewport.y, colW, viewport.height}, colTop, date);
    drawIcon(renderer, static_cast<uint8_t>(i), cx + (colW - iconBox) / 2, colTop + 22, iconBox);

    char range[24];
    formatTempRange(day, range, sizeof(range));
    drawCentered(renderer, NOTOSANS_12_FONT_ID, Rect{cx, viewport.y, colW, viewport.height}, colTop + 108, day.type);
    drawCentered(renderer, SMALL_FONT_ID, Rect{cx, viewport.y, colW, viewport.height}, colTop + 132, range);
    drawCentered(renderer, SMALL_FONT_ID, Rect{cx, viewport.y, colW, viewport.height}, colTop + 152, day.fl);
  }
}
