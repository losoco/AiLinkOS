#include "RecorderActivity.h"

#if CROSSPOINT_CAP_VOICE_MEMO

#include <HalDisplay.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "CrossPointSettings.h"
#include "components/UITheme.h"
#include "fontIds.h"
#if CROSSPOINT_CAP_MUSIC_PLAYER
#include <MusicSession.h>
#endif

namespace {

// Must match lib/SloppyDigits/SloppyDigitAlphabets.h: the layout below mirrors
// sloppy::draw() so the colons land inside the inter-digit gaps.
constexpr int kSloppyDigitW = 100;
constexpr int kSloppyDigitH = 160;
constexpr float kSloppyPad = 0.92f;

constexpr uint32_t kOneHourMs = 60UL * 60UL * 1000UL;

void drawOptionRow(const GfxRenderer& renderer, const Rect& row, const char* label, const char* value) {
  renderer.drawRect(row.x, row.y, row.width, row.height, 2, true);
  const int textHeight = renderer.getTextHeight(UI_12_FONT_ID);
  const int textY = row.y + (row.height - textHeight) / 2;
  renderer.drawText(UI_12_FONT_ID, row.x + 14, textY, label, true);
  const int valueWidth = renderer.getTextWidth(UI_12_FONT_ID, value, EpdFontFamily::BOLD);
  renderer.drawText(UI_12_FONT_ID, row.x + row.width - valueWidth - 14, textY, value, true, EpdFontFamily::BOLD);
}

}  // namespace

void RecorderActivity::onEnter() {
  Activity::onEnter();
  timerStyle_ = sloppy::Style{sloppy::AlphabetId::Geometric, 0.0f, 6, 0.0f, 0.0f, 22, true};
  sloppy::prepareSeeds(/*seed=*/1u, timerStyle_, timerSeeds_);

  if (autoStart_) startRecording();
  requestUpdate();
}

void RecorderActivity::onExit() {
  // Any exit path (Back, a replacement activity, sleep) must finalize the WAV
  // instead of leaving an unpatched header on the card.
  if (session_.isRecording()) session_.end();
  Activity::onExit();
}

bool RecorderActivity::preventAutoSleep() {
  return session_.isRecording() || session_.state() == RecordingSession::State::Saving;
}

void RecorderActivity::prepareForSleep() {
  if (session_.isRecording()) session_.end();
}

void RecorderActivity::startRecording() {
  const uint16_t rate = SETTINGS.recorderHighQuality ? 32000 : 16000;
  const uint32_t limitMs = SETTINGS.recorderNoLimit ? 0 : kOneHourMs;
#if CROSSPOINT_CAP_MUSIC_PLAYER
  // Capture rebuilds the shared I2S peripheral; leaving the music session in a
  // playing state would make its auto-advance react to a silent output.
  if (MUSIC_SESSION.isActive()) MUSIC_SESSION.stop();
#endif
  if (!session_.begin(rate, limitMs)) {
    LOG_ERR("REC", "Cannot start recording (error=%u)", static_cast<unsigned>(session_.error()));
    requestUpdate();
    return;
  }
  lastShownSecond_ = -1;
  requestUpdate();
}

void RecorderActivity::loop() {
  if (session_.isRecording()) {
    const bool stopped = session_.pump();
    const int second = static_cast<int>(session_.elapsedMs() / 1000);
    if (second != lastShownSecond_) {
      lastShownSecond_ = second;
      requestUpdate();
    }
    if (stopped) {
      // The duration limit or a dead input already finalized the file.
      finish();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      if (!session_.end()) LOG_ERR("REC", "Failed to finalize recording");
      finish();
    }
    return;
  }

  switch (session_.state()) {
    case RecordingSession::State::Saving:
      return;
    case RecordingSession::State::Error:
      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        finish();
      } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        startRecording();
      }
      return;
    case RecordingSession::State::Idle:
      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        finish();
      } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        startRecording();
      } else if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
        SETTINGS.recorderHighQuality = SETTINGS.recorderHighQuality ? 0 : 1;
        if (!SETTINGS.saveToFile()) LOG_ERR("REC", "Failed to save recorder quality");
        requestUpdate();
      } else if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
        SETTINGS.recorderNoLimit = SETTINGS.recorderNoLimit ? 0 : 1;
        if (!SETTINGS.saveToFile()) LOG_ERR("REC", "Failed to save recorder limit");
        requestUpdate();
      }
      return;
    case RecordingSession::State::Recording:
      return;
  }
}

const char* RecorderActivity::statusText() const {
  switch (session_.state()) {
    case RecordingSession::State::Idle:
      return tr(STR_REC_READY);
    case RecordingSession::State::Recording:
      return tr(STR_REC_RECORDING);
    case RecordingSession::State::Saving:
      return tr(STR_REC_SAVING);
    case RecordingSession::State::Error:
      switch (session_.error()) {
        case RecordingSession::Error::NoSd:
          return tr(STR_REC_NO_SD);
        case RecordingSession::Error::MicFailed:
          return tr(STR_REC_MIC_FAILED);
        case RecordingSession::Error::WriteFailed:
          return tr(STR_REC_WRITE_FAILED);
        case RecordingSession::Error::OpenFailed:
        case RecordingSession::Error::None:
        default:
          return tr(STR_REC_OPEN_FAILED);
      }
  }
  return tr(STR_REC_READY);
}

void RecorderActivity::drawTimer(const Rect& bounds, const uint32_t seconds) const {
  char digits[10];
  int colons[2] = {0, 0};
  int colonCount = 0;

  if (seconds >= 3600) {
    unsigned hours = static_cast<unsigned>(seconds / 3600);
    if (hours > 99) hours = 99;
    snprintf(digits, sizeof(digits), "%02u%02u%02u", hours, static_cast<unsigned>((seconds / 60) % 60),
             static_cast<unsigned>(seconds % 60));
    colons[0] = 2;
    colons[1] = 4;
    colonCount = 2;
  } else {
    snprintf(digits, sizeof(digits), "%02u%02u", static_cast<unsigned>((seconds / 60) % 60),
             static_cast<unsigned>(seconds % 60));
    colons[0] = 2;
    colonCount = 1;
  }

  sloppy::draw(renderer, timerStyle_, timerSeeds_, digits,
               sloppy::Bounds{bounds.x, bounds.y, bounds.width, bounds.height});

  const int count = static_cast<int>(strlen(digits));
  const int gap = timerStyle_.digitGap;
  const int rowWidth = count * kSloppyDigitW + (count - 1) * gap;
  const float scale = std::min(static_cast<float>(bounds.width) * kSloppyPad / rowWidth,
                               static_cast<float>(bounds.height) * kSloppyPad / kSloppyDigitH);
  const int originX = bounds.x + (bounds.width - static_cast<int>(rowWidth * scale)) / 2;
  const int originY = bounds.y + (bounds.height - static_cast<int>(kSloppyDigitH * scale)) / 2;
  const int dotSize = std::max(4, static_cast<int>(7.0f * scale));

  for (int i = 0; i < colonCount; ++i) {
    const int after = colons[i];
    const int centerX = originX + static_cast<int>((after * kSloppyDigitW + (after - 1) * gap + gap / 2) * scale);
    for (const float fraction : {0.36f, 0.64f}) {
      const int centerY = originY + static_cast<int>(kSloppyDigitH * scale * fraction);
      renderer.fillRect(centerX - dotSize / 2, centerY - dotSize / 2, dotSize, dotSize, true);
    }
  }
}

void RecorderActivity::drawOptions(const Rect& bounds) const {
  const int rowHeight = 46;
  const int gap = 12;
  const Rect quality{bounds.x, bounds.y, bounds.width, rowHeight};
  const Rect limit{bounds.x, bounds.y + rowHeight + gap, bounds.width, rowHeight};
  drawOptionRow(renderer, quality, tr(STR_REC_QUALITY),
                SETTINGS.recorderHighQuality ? tr(STR_REC_QUALITY_HIGH) : tr(STR_REC_QUALITY_STANDARD));
  drawOptionRow(renderer, limit, tr(STR_REC_LIMIT),
                SETTINGS.recorderNoLimit ? tr(STR_REC_LIMIT_UNLIMITED) : tr(STR_REC_LIMIT_1H));
}

void RecorderActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int pad = metrics.contentSidePadding;
  const int top = safe.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const Rect content{safe.x + pad, top, std::max(1, safe.width - pad * 2),
                     std::max(1, safe.y + safe.height - top - metrics.verticalSpacing)};

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{safe.x, safe.y + metrics.topPadding, safe.width, metrics.headerHeight},
                 tr(STR_REC_TITLE));

  const int timerHeight = std::min(190, std::max(96, content.height * 2 / 5));
  const Rect timerRect{content.x, content.y + 8, content.width, timerHeight};
  const bool failed = session_.state() == RecordingSession::State::Error;

  if (failed) {
    UITheme::drawCenteredWrappedText(renderer, timerRect, UI_12_FONT_ID, statusText(), 2);
  } else {
    drawTimer(timerRect, session_.elapsedMs() / 1000);
  }

  const int statusY = timerRect.y + timerRect.height + 10;
  renderer.drawCenteredText(UI_12_FONT_ID, statusY, statusText(), true, EpdFontFamily::BOLD);

  if (session_.isRecording()) {
    const char* name = strrchr(session_.path(), '/');
    name = name != nullptr ? name + 1 : session_.path();
    char label[40];
    snprintf(label, sizeof(label), "%s", name);
    char* dot = strrchr(label, '.');
    if (dot != nullptr) *dot = '\0';
    renderer.drawCenteredText(UI_10_FONT_ID, statusY + renderer.getLineHeight(UI_12_FONT_ID) + 6, label);
  } else if (!failed) {
    const int optionsTop = statusY + renderer.getLineHeight(UI_12_FONT_ID) + 22;
    if (optionsTop + 46 * 2 + 12 <= content.y + content.height) {
      drawOptions(Rect{content.x, optionsTop, content.width, 46 * 2 + 12});
    }
  }

  const auto labels = session_.isRecording()
                          ? mappedInput.mapLabels(tr(STR_BACK), tr(STR_REC_STOP), "", "")
                          : mappedInput.mapLabels(tr(STR_BACK), failed ? tr(STR_RETRY) : tr(STR_REC_START),
                                                  failed ? "" : tr(STR_REC_QUALITY), failed ? "" : tr(STR_REC_LIMIT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

#endif  // CROSSPOINT_CAP_VOICE_MEMO
