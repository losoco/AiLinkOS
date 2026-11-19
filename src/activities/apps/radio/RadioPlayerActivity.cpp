#include "RadioPlayerActivity.h"

#if CROSSPOINT_CAP_RADIO

#include <HalDisplay.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>

#include "CrossPointSettings.h"
#include "components/UITheme.h"

namespace {

constexpr uint32_t kVolumeOverlayMs = 3000;
constexpr int kVolumeStep = 5;

const char* statusLabel(const RadioSession::Status status) {
  switch (status) {
    case RadioSession::Status::Connecting:
      return tr(STR_RADIO_CONNECTING);
    case RadioSession::Status::Playing:
      return tr(STR_RADIO_PLAYING);
    case RadioSession::Status::Paused:
      return tr(STR_RADIO_PAUSED);
    case RadioSession::Status::Error:
      return tr(STR_RADIO_ERROR);
    case RadioSession::Status::Stopped:
    case RadioSession::Status::Ready:
    case RadioSession::Status::Empty:
      return tr(STR_RADIO_STOPPED);
  }
  return "";
}

void drawCenteredLine(const GfxRenderer& renderer, const Rect& bounds, const int fontId, const int y, const char* text,
                      const EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
  const int width = renderer.getTextWidth(fontId, text, style);
  renderer.drawText(fontId, bounds.x + (bounds.width - width) / 2, y, text, true, style);
}

}  // namespace

void RadioPlayerActivity::onEnter() {
  Activity::onEnter();
  RADIO_SESSION.attachRenderer(&renderer);
  previousOrientation_ = renderer.getOrientation();
  renderer.setOrientation(
      static_cast<GfxRenderer::Orientation>(SETTINGS.musicOrientation % CrossPointSettings::ORIENTATION_COUNT));
  lastStatus_ = RADIO_SESSION.status();
  lastIndex_ = RADIO_SESSION.currentIndex();
  volumeOverlay_ = false;
  fullDirty_ = true;
  requestUpdate();
}

void RadioPlayerActivity::loop() {
  const RadioSession::Status status = RADIO_SESSION.status();
  const int index = RADIO_SESSION.currentIndex();
  if (status != lastStatus_ || index != lastIndex_) {
    stationDirty_ = index != lastIndex_;
    lastStatus_ = status;
    lastIndex_ = index;
    fullDirty_ = true;
    requestUpdate();
  }

  if (volumeOverlay_ && millis() >= overlayDeadlineMs_) {
    volumeOverlay_ = false;
    volumeDirty_ = true;
    requestUpdate();
  }

  // Held direction keys arrive as Up/Down on this panel; one hold, one station.
  if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
    RADIO_SESSION.previous();
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
    RADIO_SESSION.next();
    return;
  }

  if (mappedInput.wasLongPressed(MappedInputManager::Button::Back, 600)) {
    RADIO_SESSION.stop();
    finish();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    // Keep the station on the air; the list shows it as playing.
    finish();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    RADIO_SESSION.togglePause();
    fullDirty_ = true;
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    changeVolume(-kVolumeStep);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    changeVolume(kVolumeStep);
    return;
  }
}

bool RadioPlayerActivity::preventAutoSleep() { return RADIO_SESSION.isPlaying(); }

void RadioPlayerActivity::prepareForSleep() { RADIO_SESSION.stop(); }

void RadioPlayerActivity::onExit() {
  renderer.setOrientation(previousOrientation_);
  Activity::onExit();
}

void RadioPlayerActivity::changeVolume(const int delta) {
  const int next = std::clamp(static_cast<int>(RADIO_SESSION.volume()) + delta, 0, 100);
  RADIO_SESSION.setVolume(static_cast<uint8_t>(next));
  SETTINGS.musicVolume = static_cast<uint8_t>(next);
  volumeOverlay_ = true;
  overlayDeadlineMs_ = millis() + kVolumeOverlayMs;
  volumeDirty_ = true;
  fullDirty_ = true;
  requestUpdate();
}

void RadioPlayerActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int contentTop = safe.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int footerReserve = GUI.hintsRowDrawn() ? metrics.buttonHintsHeight : 0;
  const int contentBottom = safe.y + safe.height - footerReserve - metrics.verticalSpacing;
  const Rect content{safe.x + metrics.contentSidePadding, contentTop,
                     std::max(1, safe.width - metrics.contentSidePadding * 2), std::max(1, contentBottom - contentTop)};

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{safe.x, safe.y + metrics.topPadding, safe.width, metrics.headerHeight},
                 tr(STR_RADIO_TITLE));

  const RadioStation* station = RADIO_SESSION.stationAt(static_cast<size_t>(std::max(0, RADIO_SESSION.currentIndex())));
  const int smallLh = renderer.getLineHeight(SMALL_FONT_ID);
  const int titleLh = renderer.getLineHeight(UI_12_FONT_ID);
  const int gap = std::max(8, static_cast<int>(metrics.verticalSpacing));

  // Station name owns the upper half; the state block sits below it.
  const Rect nameRect{content.x, content.y + content.height * 12 / 100, content.width, titleLh * 2};
  UITheme::drawCenteredWrappedText(renderer, nameRect, UI_12_FONT_ID, station ? station->name : "", 2, true,
                                   EpdFontFamily::BOLD, UITheme::TextVerticalAlignment::CENTER);

  if (station) {
    drawCenteredLine(renderer, content, SMALL_FONT_ID, nameRect.y + nameRect.height + gap, station->category);
  }

  // State: a filled square for a live stream, plain text otherwise, so the page
  // can be read from across the room.
  const RadioSession::Status status = RADIO_SESSION.status();
  const char* label = statusLabel(status);
  const int statusY = content.y + content.height * 58 / 100;
  const int labelWidth = renderer.getTextWidth(SMALL_FONT_ID, label);
  const bool live = status == RadioSession::Status::Playing;
  const int dot = live ? 10 : 0;
  const int startX = content.x + (content.width - labelWidth - (dot ? dot + 10 : 0)) / 2;
  if (dot) {
    renderer.fillRect(startX, statusY + 4, dot, dot, true);
    renderer.drawText(SMALL_FONT_ID, startX + dot + 10, statusY, label, true);
  } else {
    renderer.drawText(SMALL_FONT_ID, startX, statusY, label, true);
  }

  // A failed connection explains itself instead of leaving "连接失败" alone.
  if (status == RadioSession::Status::Error && RADIO_SESSION.errorText()[0]) {
    drawCenteredLine(renderer, content, SMALL_FONT_ID, statusY + smallLh + gap, RADIO_SESSION.errorText());
  }

  if (volumeOverlay_) drawVolumeOverlay(content, RADIO_SESSION.volume());

  const char* playLabel = live ? tr(STR_RADIO_PAUSE) : tr(STR_RADIO_PLAY);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), playLabel, tr(STR_MUSIC_VOL_DOWN), tr(STR_MUSIC_VOL_UP));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  const bool halfFrame = stationDirty_;
  stationDirty_ = false;
  if (halfFrame) {
    renderer.promoteNextRefresh(HalDisplay::HALF_REFRESH);
  }
  LOG_INF("RAD", "Flush mode=%s status=%u idx=%d", halfFrame ? "HALF" : "FAST", static_cast<unsigned>(status),
          RADIO_SESSION.currentIndex());
  fullDirty_ = false;
  volumeDirty_ = false;
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

void RadioPlayerActivity::drawVolumeOverlay(const Rect& content, const uint8_t volume) {
  const int panelWidth = std::max(260, content.width * 2 / 3);
  const int panelHeight = 110;
  const int px = content.x + (content.width - panelWidth) / 2;
  const int py = content.y + content.height - panelHeight - 8;

  renderer.fillRect(px, py, panelWidth, panelHeight, false);
  renderer.drawRect(px, py, panelWidth, panelHeight, true);

  char percent[8] = {};
  snprintf(percent, sizeof(percent), "%u%%", static_cast<unsigned>(volume));
  renderer.drawText(SMALL_FONT_ID, px + 16, py + 14, tr(STR_MUSIC_VOLUME), true);
  const int percentWidth = renderer.getTextWidth(SMALL_FONT_ID, percent);
  renderer.drawText(SMALL_FONT_ID, px + panelWidth - 16 - percentWidth, py + 14, percent, true);

  const Rect bar{px + 16, py + 52, panelWidth - 32, 16};
  renderer.drawRect(bar.x, bar.y, bar.width, bar.height, true);
  const int filled = static_cast<int>((bar.width - 4) * volume / 100);
  if (filled > 0) renderer.fillRect(bar.x + 2, bar.y + 2, filled, bar.height - 4, true);
}

#endif  // CROSSPOINT_CAP_RADIO
