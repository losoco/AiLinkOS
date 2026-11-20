#include "RadioListActivity.h"

#if CROSSPOINT_CAP_RADIO

#include <HalDisplay.h>
#include <I18n.h>
#include <Logging.h>

#include <cstring>

#include "CrossPointSettings.h"
#include "RadioPlayerActivity.h"
#include "RadioSession.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"

namespace fui = freeink::ui;

int RadioListActivity::listCount() const { return static_cast<int>(RADIO_SESSION.stationCount()); }

void RadioListActivity::onEnter() {
  UiListActivity::onEnter();
  RADIO_SESSION.attachRenderer(&renderer);
  // Music and radio share musicVolume; adopt the persisted level unless a live
  // session already owns the output.
  if (!RADIO_SESSION.isActive()) RADIO_SESSION.setVolume(SETTINGS.musicVolume);
  if (RADIO_SESSION.stationCount() == 0) {
    rescan();
  } else {
    // Coming back from the player: keep the highlighted row.
    const int current = RADIO_SESSION.currentIndex();
    if (current >= 0) nav.selected = current;
    nav.followOnBuild = true;
  }
  requestUpdate();
}

void RadioListActivity::loop() {
  // A dead stream is detected on the audio task; reflect it in the row badge.
  const RadioSession::Status before = RADIO_SESSION.status();
  RADIO_SESSION.poll();
  if (RADIO_SESSION.status() != before) requestUpdate();
  UiListActivity::loop();
}

void RadioListActivity::rescan() {
  RADIO_SESSION.setVolume(SETTINGS.musicVolume);
  loadResult_ = RADIO_SESSION.loadStations();
  nav.selected = 0;
  nav.top = 0;
  nav.followOnBuild = true;
  if (loadResult_ != RadioStations::LoadResult::Ok) {
    LOG_INF("RAD", "Scan finished with result %d", static_cast<int>(loadResult_));
  }
}

void RadioListActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect content = pageContentRect();
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(content.y), static_cast<int16_t>(renderer.getScreenWidth() - content.x - content.width),
      static_cast<int16_t>(renderer.getScreenHeight() - content.y - content.height), static_cast<int16_t>(content.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  if (RADIO_SESSION.stationCount() == 0) {
    const char* message = tr(STR_RADIO_EMPTY);
    if (loadResult_ == RadioStations::LoadResult::NoSd) {
      message = tr(STR_RADIO_NO_SD);
    } else if (loadResult_ == RadioStations::LoadResult::NoFolder) {
      message = tr(STR_RADIO_NO_FOLDER);
    }
    screen.centeredText(message, screen.theme().bodyText);
    return;
  }

  fui::ListProps props;
  props.count = static_cast<uint16_t>(RADIO_SESSION.stationCount());
  props.action = ACTION_ROW;
  props.rowProvider = &RadioListActivity::provideRow;
  props.rowProviderCtx = this;
  props.valueInset = 8;
  props.balanceWrappedLabelWithValue = false;
  props.labelText = UITheme::getInstance().hasMainTabs() ? screen.theme().bodyText : screen.theme().smallText;
  props.labelText.maxLines = 2;

  syncListViewport(screen, props);
  screen.list(props);
}

void RadioListActivity::provideRow(void* ctx, const uint16_t index, freeink::ui::ListItem& item) {
  auto* self = static_cast<RadioListActivity*>(ctx);
  const RadioStation* station = RADIO_SESSION.stationAt(index);
  if (!station) return;

  snprintf(self->rowName_, sizeof(self->rowName_), "%s", station->name);
  item.label = self->rowName_;
  // The category repeats under each station instead of a second header row: the
  // list is flat, and the imported files already provide a meaningful grouping.
  snprintf(self->rowSubtitle_, sizeof(self->rowSubtitle_), "%s", station->category);
  item.subtitle = self->rowSubtitle_;
  // Touch hit rects are registered with this value; without it every tap would
  // select the first station (the music list had exactly that bug).
  item.actionValue = static_cast<int16_t>(index);

  if (static_cast<int>(index) == RADIO_SESSION.currentIndex()) {
    if (RADIO_SESSION.isPlaying()) {
      item.value = tr(STR_RADIO_PLAYING);
    } else if (RADIO_SESSION.isPaused()) {
      item.value = tr(STR_RADIO_PAUSED);
    } else if (RADIO_SESSION.status() == RadioSession::Status::Error) {
      item.value = tr(STR_RADIO_ERROR);
    }
  }
  item.icon = listIconFor(UIIcon::Radio, 32);
}

void RadioListActivity::activateIndex(const int index) {
  if (index < 0 || index >= static_cast<int>(RADIO_SESSION.stationCount())) return;
  app.clearTapFlash();
  // Re-opening the station that is already playing must not restart it.
  if (!(index == RADIO_SESSION.currentIndex() && RADIO_SESSION.isActive())) {
    RADIO_SESSION.playIndex(index);
  }
  startActivityForResultWith<RadioPlayerActivity>([this](const ActivityResult&) {
    renderer.promoteNextRefresh(HalDisplay::HALF_REFRESH);
    requestUpdate();
  });
}

const char* RadioListActivity::headerTitle() const { return tr(STR_RADIO_TITLE); }

bool RadioListActivity::handleCustomInput() {
  // Hold Back to re-import /radio after editing the list on a computer.
  if (mappedInput.wasLongPressed(MappedInputManager::Button::Back, 600)) {
    rescan();
    renderer.promoteNextRefresh(HalDisplay::HALF_REFRESH);
    requestUpdate();
    return true;
  }
  return false;
}

#endif  // CROSSPOINT_CAP_RADIO
