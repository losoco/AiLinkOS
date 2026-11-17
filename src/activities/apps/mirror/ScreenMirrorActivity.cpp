#include "ScreenMirrorActivity.h"

#if CROSSPOINT_CAP_SCREEN_MIRROR

#include <I18n.h>
#include <Logging.h>

#include <cstdio>

#include "components/UITheme.h"
#include "fontIds.h"
#include "network/ScreenMirrorService.h"

namespace {

void drawCentered(const GfxRenderer& renderer, const int fontId, const int y, const char* text,
                  const EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
  const int width = renderer.getTextWidth(fontId, text, style);
  renderer.drawText(fontId, (renderer.getScreenWidth() - width) / 2, y, text, true, style);
}

}  // namespace

bool ScreenMirrorActivity::preventAutoSleep() { return SCREEN_MIRROR.enabled(); }

bool ScreenMirrorActivity::skipLoopDelay() {
  return SCREEN_MIRROR.state() == ScreenMirrorService::State::WifiConnecting;
}

void ScreenMirrorActivity::onEnter() {
  Activity::onEnter();
  lastState_ = -1;
  lastClients_ = 0xff;
  requestUpdate();
}

void ScreenMirrorActivity::loop() {
  const auto state = static_cast<int>(SCREEN_MIRROR.state());
  // Repaint only on state or viewer-count transitions. Watching the frame
  // counter here would feed the mirror its own repaint: render -> framebuffer
  // commit -> new frame -> repaint, which would keep the panel refreshing.
  if (state != lastState_ || SCREEN_MIRROR.clientCount() != lastClients_) {
    lastState_ = state;
    lastClients_ = SCREEN_MIRROR.clientCount();
    requestUpdate();
  }

  // Long Back stops the service and leaves; short Back only leaves, so the
  // mirror can keep following the device while the user reads.
  if (mappedInput.wasLongPressed(MappedInputManager::Button::Back, 600)) {
    SCREEN_MIRROR.stop();
    finish();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (SCREEN_MIRROR.enabled()) {
      SCREEN_MIRROR.stop();
    } else {
      SCREEN_MIRROR.start(renderer);
    }
    requestUpdate();
    return;
  }
}

void ScreenMirrorActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{safe.x, safe.y + metrics.topPadding, safe.width, metrics.headerHeight},
                 tr(STR_SCREEN_MIRROR_TITLE));

  const auto state = SCREEN_MIRROR.state();
  const char* status = tr(STR_MIRROR_OFF);
  switch (state) {
    case ScreenMirrorService::State::Off:
      status = tr(STR_MIRROR_OFF);
      break;
    case ScreenMirrorService::State::WifiConnecting:
      status = tr(STR_MIRROR_CONNECTING);
      break;
    case ScreenMirrorService::State::Running:
      status = tr(STR_MIRROR_RUNNING);
      break;
    case ScreenMirrorService::State::Error:
      status = tr(STR_MIRROR_ERROR);
      break;
  }

  const int lineH = renderer.getLineHeight(SMALL_FONT_ID);
  int y = safe.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing + 12;

  drawCentered(renderer, UI_12_FONT_ID, y, status);
  y += renderer.getLineHeight(UI_12_FONT_ID) + 16;

  if (state == ScreenMirrorService::State::Running) {
    char line[96];
    std::snprintf(line, sizeof(line), "http://ailinkos.local");
    drawCentered(renderer, NOTOSANS_12_FONT_ID, y, line);
    y += lineH + 6;
    std::snprintf(line, sizeof(line), "%s %s", tr(STR_MIRROR_URL), SCREEN_MIRROR.ip().toString().c_str());
    drawCentered(renderer, SMALL_FONT_ID, y, line);
    y += lineH + 20;

    std::snprintf(line, sizeof(line), "%s", tr(STR_MIRROR_PIN));
    drawCentered(renderer, SMALL_FONT_ID, y, line);
    y += lineH + 4;
    drawCentered(renderer, NOTOSANS_18_FONT_ID, y, SCREEN_MIRROR.pin());
    y += renderer.getLineHeight(NOTOSANS_18_FONT_ID) + 16;

    std::snprintf(line, sizeof(line), tr(STR_MIRROR_CLIENTS_FMT), static_cast<unsigned>(SCREEN_MIRROR.clientCount()));
    drawCentered(renderer, SMALL_FONT_ID, y, line);
  } else if (state == ScreenMirrorService::State::WifiConnecting) {
    drawCentered(renderer, SMALL_FONT_ID, y, SCREEN_MIRROR.ip().toString().c_str());
  } else {
    drawCentered(renderer, SMALL_FONT_ID, y, tr(STR_MIRROR_HINT));
  }

  const char* confirmLabel = SCREEN_MIRROR.enabled() ? tr(STR_MIRROR_STOP) : tr(STR_MIRROR_START);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

#endif  // CROSSPOINT_CAP_SCREEN_MIRROR
