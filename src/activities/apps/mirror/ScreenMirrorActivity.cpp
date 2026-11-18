#include "ScreenMirrorActivity.h"

#if CROSSPOINT_CAP_SCREEN_MIRROR

#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>
#include <string>

#include "components/UITheme.h"
#include "fontIds.h"
#include "network/ScreenMirrorService.h"
#include "util/QrUtils.h"

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

  // Back only leaves; the mirror keeps following the device so the user can read
  // on. Toggling lives on Left/Right: Back is a multi-click gesture on this
  // board (single = confirm, double = back) and easily stops the service by
  // accident, and a single wheel nudge lands on Left/Right.
  // Left/Right must come from the panel itself: the viewer page's own arrows and
  // ▶ button send the same keys, and a remote Right would otherwise close the
  // service the viewer is watching.
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  if (mappedInput.wasLocalReleased(MappedInputManager::Button::Left) && !SCREEN_MIRROR.enabled()) {
    SCREEN_MIRROR.start(renderer);
    requestUpdate();
    return;
  }
  if (mappedInput.wasLocalReleased(MappedInputManager::Button::Right) && SCREEN_MIRROR.enabled()) {
    SCREEN_MIRROR.stop("activity-right");
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
    // Phones scan straight into the viewer page, like AirPage's QR screen. The
    // IP form is encoded because some Android browsers refuse to resolve .local;
    // the hostname stays below as text for people who type the address.
    char url[48];
    std::snprintf(url, sizeof(url), "http://%s:%u", SCREEN_MIRROR.ip().toString().c_str(),
                  static_cast<unsigned>(ScreenMirrorService::kHttpPort));

    const bool showPin = SCREEN_MIRROR.pinRequired();
    const int pinHeight = showPin ? lineH + 4 + renderer.getLineHeight(NOTOSANS_18_FONT_ID) : 0;
    const int textHeight = 12 + lineH + 4 + lineH + pinHeight + 16 + lineH;
    // Keep the QR above the key-hint and button rows. Cap it at 80% of the panel
    // width so the modules keep a white quiet zone on both sides: a code that
    // runs into the screen edge is what makes phone scanners give up.
    const int bottomLimit = safe.y + safe.height - metrics.buttonHintsHeight - lineH - 8;
    const int qrSize = std::clamp(
        std::min({safe.width - 24, bottomLimit - y - textHeight - 12, renderer.getScreenWidth() * 4 / 5}), 96, 384);

    const Rect qrBounds{(renderer.getScreenWidth() - qrSize) / 2, y, qrSize, qrSize};
    QrUtils::drawQrCode(renderer, qrBounds, url);
    y = qrBounds.y + qrSize + 12;

    char line[96];
    std::snprintf(line, sizeof(line), "http://ailinkos.local:%u",
                  static_cast<unsigned>(ScreenMirrorService::kHttpPort));
    drawCentered(renderer, NOTOSANS_12_FONT_ID, y, line);
    y += lineH + 4;
    std::snprintf(line, sizeof(line), "%s %s", tr(STR_MIRROR_URL), SCREEN_MIRROR.ip().toString().c_str());
    drawCentered(renderer, SMALL_FONT_ID, y, line);
    y += lineH + 4;

    // The PIN row only exists while Settings → System → Mirror PIN is on.
    if (showPin) {
      std::snprintf(line, sizeof(line), "%s", tr(STR_MIRROR_PIN));
      drawCentered(renderer, SMALL_FONT_ID, y, line);
      y += lineH + 4;
      drawCentered(renderer, NOTOSANS_18_FONT_ID, y, SCREEN_MIRROR.pin());
      y += renderer.getLineHeight(NOTOSANS_18_FONT_ID) + 16;
    }

    std::snprintf(line, sizeof(line), tr(STR_MIRROR_CLIENTS_FMT), static_cast<unsigned>(SCREEN_MIRROR.clientCount()));
    drawCentered(renderer, SMALL_FONT_ID, y, line);
  } else if (state == ScreenMirrorService::State::WifiConnecting) {
    drawCentered(renderer, SMALL_FONT_ID, y, SCREEN_MIRROR.ip().toString().c_str());
  } else {
    drawCentered(renderer, SMALL_FONT_ID, y, tr(STR_MIRROR_HINT));
  }

  // Spell out the wheel mapping above the button row: the footer labels alone
  // do not say which key starts and which stops.
  const int helpLineHeight = renderer.getLineHeight(SMALL_FONT_ID);
  GUI.drawHelpText(renderer,
                   Rect{safe.x, safe.y + safe.height - metrics.buttonHintsHeight - helpLineHeight, safe.width,
                        helpLineHeight},
                   tr(STR_MIRROR_KEY_HINT));

  // Left starts, Right stops; the confirm slot is intentionally unused.
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", tr(STR_MIRROR_START), tr(STR_MIRROR_STOP));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

#endif  // CROSSPOINT_CAP_SCREEN_MIRROR
