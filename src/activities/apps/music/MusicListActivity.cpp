#include "MusicListActivity.h"

#if CROSSPOINT_CAP_MUSIC_PLAYER

#include <HalDisplay.h>
#include <I18n.h>
#include <Logging.h>

#include "CrossPointSettings.h"
#include "MusicPlayerActivity.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "util/MusicCoverLoader.h"

namespace fui = freeink::ui;

void MusicListActivity::onEnter() {
  // Follow the live session: a background track may have auto-advanced while
  // this screen was closed, so the highlighted row must track what is audible.
  const int sessionIndex = MUSIC_SESSION.currentIndex();
  const int previousSelection = sessionIndex >= 0 ? sessionIndex : nav.selected.load();

  // The session is a plain lib and does not read settings itself; the UI owns
  // the persisted volume.
  MUSIC_SESSION.setVolume(SETTINGS.musicVolume);

  UiListActivity::onEnter();

  if (MUSIC_SESSION.trackCount() == 0) {
    rescan();
  } else {
    // Coming back from the player: keep the row the user was on instead of
    // resetting the list to the first entry.
    nav.selected = previousSelection;
    nav.followOnBuild = true;
  }
  requestUpdate();
}

void MusicListActivity::loop() {
  const int indexBefore = MUSIC_SESSION.currentIndex();
  const MusicSession::Status statusBefore = MUSIC_SESSION.status();

  // Auto-advance and decode errors surface while this screen is visible, so the
  // "playing" badge has to follow the session without user input. This is a
  // track change, so it must not flash the panel: the default list repaint is a
  // FAST frame, and the old HALF promotion here would have been a full-screen
  // OTP waveform in the middle of browsing.
  MUSIC_SESSION.poll();
  if (MUSIC_SESSION.currentIndex() != indexBefore || MUSIC_SESSION.status() != statusBefore) {
    requestUpdate();
  }

  UiListActivity::loop();
}

void MusicListActivity::rescan() {
  loadResult_ = MUSIC_SESSION.loadFolder();
  nav.selected = 0;
  nav.top = 0;
  nav.followOnBuild = true;
  if (loadResult_ != MusicSession::LoadResult::Ok) {
    LOG_INF("MUS", "Scan finished with result %d", static_cast<int>(loadResult_));
  }
  // Cover caching is keyed by file identity, so stale entries are only useful
  // until the library changes. Trim them here instead of on every screen entry.
  MusicCoverLoader::pruneCache();
}

void MusicListActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect content = pageContentRect();
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(content.y), static_cast<int16_t>(renderer.getScreenWidth() - content.x - content.width),
      static_cast<int16_t>(renderer.getScreenHeight() - content.y - content.height), static_cast<int16_t>(content.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  if (MUSIC_SESSION.trackCount() == 0) {
    const char* message = tr(STR_MUSIC_EMPTY);
    if (loadResult_ == MusicSession::LoadResult::NoSd) {
      message = tr(STR_MUSIC_NO_SD);
    } else if (loadResult_ == MusicSession::LoadResult::NoFolder) {
      message = tr(STR_MUSIC_NO_FOLDER);
    }
    screen.centeredText(message, screen.theme().bodyText);
    return;
  }

  fui::ListProps props;
  props.count = static_cast<uint16_t>(MUSIC_SESSION.trackCount());
  props.action = ACTION_ROW;
  props.rowProvider = &MusicListActivity::provideRow;
  props.rowProviderCtx = this;
  props.valueInset = 8;
  props.balanceWrappedLabelWithValue = false;
  props.labelText = UITheme::getInstance().hasMainTabs() ? screen.theme().bodyText : screen.theme().smallText;
  props.labelText.maxLines = 2;

  syncListViewport(screen, props);
  screen.list(props);
}

void MusicListActivity::provideRow(void* ctx, const uint16_t index, freeink::ui::ListItem& item) {
  auto* self = static_cast<MusicListActivity*>(ctx);
  MUSIC_SESSION.trackName(index, self->rowName_, sizeof(self->rowName_));
  item.label = self->rowName_;
  // Badge: current track, otherwise the container format of that row. Paused
  // and failed tracks must not keep reading as "playing" once the session
  // survives outside this screen.
  if (static_cast<int>(index) == MUSIC_SESSION.currentIndex() && MUSIC_SESSION.isPlaying()) {
    item.value = tr(STR_MUSIC_PLAYING);
  } else if (static_cast<int>(index) == MUSIC_SESSION.currentIndex() && MUSIC_SESSION.isPaused()) {
    item.value = tr(STR_MUSIC_PAUSED);
  } else {
    item.value = MUSIC_SESSION.trackFormat(index) == AudioFormat::Wav ? "WAV" : "MP3";
  }
  // 24 px rows scale the 32 px app icon down through BitmapMode::Contain.
  item.icon = listIconFor(UIIcon::Music, 32);
}

void MusicListActivity::activateIndex(const int index) {
  if (index < 0 || index >= static_cast<int>(MUSIC_SESSION.trackCount())) return;
  // Re-opening the player for the track that is already playing/paused must not
  // restart it: the session is the source of truth and the screen just attaches.
  if (!(index == MUSIC_SESSION.currentIndex() && MUSIC_SESSION.isActive())) {
    // A failed start still opens the player so its error state is visible.
    MUSIC_SESSION.playIndex(index);
  }
  app.clearTapFlash();
  startActivityForResultWith<MusicPlayerActivity>([this](const ActivityResult&) {
    renderer.promoteNextRefresh(HalDisplay::HALF_REFRESH);
    requestUpdate();
  });
}

const char* MusicListActivity::headerTitle() const { return tr(STR_MUSIC_TITLE); }

bool MusicListActivity::handleCustomInput() {
  // Hold Back to rescan /music (files can be added without leaving the app).
  if (mappedInput.wasLongPressed(MappedInputManager::Button::Back, 600)) {
    rescan();
    renderer.promoteNextRefresh(HalDisplay::HALF_REFRESH);
    requestUpdate();
    return true;
  }
  return false;
}

void MusicListActivity::onBackButton() {
  // Leaving the app no longer tears the session down: playback continues in the
  // background and re-entering restores the same track, position, and volume.
  // Only deep sleep, an explicit stop, a decode error, or the SD handoff to USB
  // releases the audio rail (see docs/engineering/music-player-ui-design.md).
  activityManager.goToApps();
}

void MusicListActivity::drawFooter() {
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_MUSIC_PLAY), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

#endif  // CROSSPOINT_CAP_MUSIC_PLAYER
