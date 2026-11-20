#include "RecordingListActivity.h"

#if CROSSPOINT_CAP_VOICE_MEMO

#include <HalDisplay.h>
#include <I18n.h>
#include <Logging.h>

#include <cstring>
#include <string>
#include <utility>

#include "CrossPointSettings.h"
#include "RecorderActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"

#if !CROSSPOINT_CAP_MUSIC_PLAYER
#error "Voice memo playback reuses the music player; enable CROSSPOINT_CAP_MUSIC_PLAYER"
#endif
#include <MusicSession.h>

#include "activities/apps/music/MusicPlayerActivity.h"

namespace fui = freeink::ui;

void RecordingListActivity::onEnter() {
  UiListActivity::onEnter();
  rescan();
  // Coming back while a memo is playing: put the cursor on it.
  if (!entries_.empty() && MUSIC_SESSION.isExternal() && MUSIC_SESSION.currentIndex() >= 0) {
    nav.selected = MUSIC_SESSION.currentIndex();
    if (nav.selected >= static_cast<int>(entries_.size())) nav.selected = static_cast<int>(entries_.size()) - 1;
  }
  nav.followOnBuild = true;
  requestUpdate();
}

void RecordingListActivity::onExit() { UiListActivity::onExit(); }

void RecordingListActivity::loop() {
  // Reaching the end of a memo stops the session; reflect the badge change.
  const int indexBefore = MUSIC_SESSION.currentIndex();
  const int statusBefore = static_cast<int>(MUSIC_SESSION.status());
  MUSIC_SESSION.poll();
  if (MUSIC_SESSION.currentIndex() != indexBefore || static_cast<int>(MUSIC_SESSION.status()) != statusBefore) {
    requestUpdate();
  }
  UiListActivity::loop();
}

void RecordingListActivity::rescan() {
  sdReady_ = recording::scan(entries_);
  if (entries_.empty()) {
    nav.selected = 0;
    nav.top = 0;
  } else {
    if (nav.selected < 0) nav.selected = 0;
    if (nav.selected >= static_cast<int>(entries_.size())) nav.selected = static_cast<int>(entries_.size()) - 1;
  }
  nav.followOnBuild = true;
}

void RecordingListActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect content = pageContentRect();
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(content.y), static_cast<int16_t>(renderer.getScreenWidth() - content.x - content.width),
      static_cast<int16_t>(renderer.getScreenHeight() - content.y - content.height), static_cast<int16_t>(content.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  if (entries_.empty()) {
    screen.centeredText(sdReady_ ? tr(STR_REC_EMPTY) : tr(STR_REC_NO_SD), screen.theme().bodyText);
    return;
  }

  fui::ListProps props;
  props.count = static_cast<uint16_t>(entries_.size());
  props.action = ACTION_ROW;
  props.rowProvider = &RecordingListActivity::provideRow;
  props.rowProviderCtx = this;
  props.valueInset = 8;
  props.balanceWrappedLabelWithValue = false;
  props.labelText = UITheme::getInstance().hasMainTabs() ? screen.theme().bodyText : screen.theme().smallText;
  props.labelText.maxLines = 1;

  syncListViewport(screen, props);
  screen.list(props);
}

void RecordingListActivity::provideRow(void* ctx, const uint16_t index, freeink::ui::ListItem& item) {
  auto* self = static_cast<RecordingListActivity*>(ctx);
  if (index >= self->entries_.size()) return;
  const RecordingEntry& entry = self->entries_[index];

  recording::formatStartTime(entry.name, self->rowName_, sizeof(self->rowName_));
  item.label = self->rowName_;
  recording::formatDuration(entry.durationMs, self->rowSubtitle_, sizeof(self->rowSubtitle_));
  item.subtitle = self->rowSubtitle_;
  item.actionValue = static_cast<int16_t>(index);

  if (MUSIC_SESSION.isExternal() && static_cast<int>(index) == MUSIC_SESSION.currentIndex()) {
    if (MUSIC_SESSION.isPlaying()) {
      item.value = tr(STR_REC_PLAYING);
    } else if (MUSIC_SESSION.isPaused()) {
      item.value = tr(STR_MUSIC_PAUSED);
    }
  }
  item.icon = listIconFor(UIIcon::VoiceMemo, 32);
}

void RecordingListActivity::activateIndex(const int index) {
  app.clearTapFlash();
  openPlayer(index);
}

void RecordingListActivity::openPlayer(const int index) {
  if (index < 0 || index >= static_cast<int>(entries_.size())) return;

  // Re-opening the memo that is already playing must not restart it; the
  // player simply attaches to the live session, exactly like the music list.
  const bool alreadyActive =
      MUSIC_SESSION.isExternal() && MUSIC_SESSION.currentIndex() == index && MUSIC_SESSION.isActive();
  if (!alreadyActive) {
    std::vector<std::string> paths;
    paths.reserve(entries_.size());
    for (const RecordingEntry& entry : entries_) paths.emplace_back(entry.path);
    if (!MUSIC_SESSION.playExternal(std::move(paths), index)) {
      LOG_ERR("REC", "Cannot play %s", entries_[index].path);
    }
  }

  const bool started = startActivityForResultWith<MusicPlayerActivity>([this](const ActivityResult&) {
    renderer.promoteNextRefresh(HalDisplay::HALF_REFRESH);
    requestUpdate();
  });
  if (!started) LOG_ERR("REC", "OOM: music player activity");
}

void RecordingListActivity::startRecorder(const bool autoStart) {
  // Capture rebuilds the shared I2S peripheral; release any memo/music first.
  if (MUSIC_SESSION.isActive()) MUSIC_SESSION.stop();
  const bool started = startActivityForResultWith<RecorderActivity>(
      [this](const ActivityResult&) {
        rescan();
        renderer.promoteNextRefresh(HalDisplay::HALF_REFRESH);
        requestUpdate();
      },
      autoStart);
  if (!started) LOG_ERR("REC", "OOM: recorder activity");
}

void RecordingListActivity::promptDelete() {
  if (entries_.empty() || nav.selected < 0 || nav.selected >= static_cast<int>(entries_.size())) return;

  const RecordingEntry entry = entries_[nav.selected];
  char title[32];
  recording::formatStartTime(entry.name, title, sizeof(title));
  const std::string path(entry.path);

  auto handler = [this, path](const ActivityResult& result) {
    if (result.isCancelled) return;
    // The external playlist keeps the deleted path until the next scan; stop
    // playback instead of letting a skip land on a missing file.
    if (MUSIC_SESSION.isExternal()) MUSIC_SESSION.stop();
    if (!recording::remove(path.c_str())) LOG_ERR("REC", "Cannot delete %s", path.c_str());
    rescan();
    requestUpdate();
  };
  if (!startActivityForResultWith<ConfirmationActivity>(std::move(handler), tr(STR_DELETE_ITEM_CONFIRM),
                                                        std::string(title))) {
    LOG_ERR("REC", "OOM: delete confirmation");
  }
}

const char* RecordingListActivity::headerTitle() const { return tr(STR_REC_TITLE); }

bool RecordingListActivity::handleCustomInput() {
  // Hold Back to pick up files copied to /rec from a computer.
  if (mappedInput.wasLongPressed(MappedInputManager::Button::Back, 900)) {
    rescan();
    renderer.promoteNextRefresh(HalDisplay::HALF_REFRESH);
    requestUpdate();
    return true;
  }
  // This panel turns a held direction key into Up/Down: Up means the left key
  // was held (delete), Down means the right key was held (new recording).
  if (mappedInput.wasLongPressed(MappedInputManager::Button::Up, 600)) {
    promptDelete();
    return true;
  }
  if (mappedInput.wasLongPressed(MappedInputManager::Button::Down, 600)) {
    startRecorder(true);
    return true;
  }
  if (mappedInput.wasLongPressed(MappedInputManager::Button::Confirm, 600)) {
    startRecorder(true);
    return true;
  }
  return false;
}

void RecordingListActivity::onBackButton() {
  // Playback keeps running in the background, like the music app; entering the
  // music list or starting a recording releases it.
  activityManager.goToApps();
}

void RecordingListActivity::drawFooter() {
  const auto labels =
      mappedInput.mapLabels(tr(STR_BACK), tr(STR_REC_PLAY), tr(STR_REC_HOLD_DELETE), tr(STR_REC_HOLD_RECORD));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

#endif  // CROSSPOINT_CAP_VOICE_MEMO
