#include "RecordingListActivity.h"

#if CROSSPOINT_CAP_VOICE_MEMO

#include <HalDisplay.h>
#include <I18n.h>
#include <Logging.h>
#include <RecordingPlayer.h>

#include <cstring>
#include <string>
#include <utility>

#include "CrossPointSettings.h"
#include "RecorderActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#if CROSSPOINT_CAP_MUSIC_PLAYER
#include <MusicSession.h>
#endif

namespace fui = freeink::ui;

namespace {

// Recorded memos are short-form playback; a fixed, comfortably audible level
// keeps the list from growing a volume control it does not need.
constexpr uint8_t kPlaybackVolume = 80;

}  // namespace

void RecordingListActivity::onEnter() {
  UiListActivity::onEnter();
  rescan();
  lastPlaying_ = RECORDING_PLAYER.isPlaying();
  // Coming back from the recorder: put the cursor on what just played, if the
  // user already started it, otherwise on the "new recording" row.
  if (RECORDING_PLAYER.currentPath()[0] != '\0') {
    for (size_t i = 0; i < entries_.size(); ++i) {
      if (strcmp(entries_[i].path, RECORDING_PLAYER.currentPath()) == 0) {
        nav.selected = static_cast<int>(i) + 1;
        break;
      }
    }
  }
  nav.followOnBuild = true;
  requestUpdate();
}

void RecordingListActivity::onExit() {
  RECORDING_PLAYER.stop();
  UiListActivity::onExit();
}

void RecordingListActivity::loop() {
  const bool wasPlaying = lastPlaying_;
  RECORDING_PLAYER.poll();
  lastPlaying_ = RECORDING_PLAYER.isPlaying();
  if (lastPlaying_ != wasPlaying) requestUpdate();
  UiListActivity::loop();
}

void RecordingListActivity::rescan() {
  sdReady_ = recording::scan(entries_);
  // Row 0 is always the "new recording" entry.
  if (nav.selected < 0) nav.selected = 0;
  if (nav.selected > static_cast<int>(entries_.size())) nav.selected = static_cast<int>(entries_.size());
  if (nav.selected < 0) nav.selected = 0;
  nav.followOnBuild = true;
}

void RecordingListActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect content = pageContentRect();
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(content.y), static_cast<int16_t>(renderer.getScreenWidth() - content.x - content.width),
      static_cast<int16_t>(renderer.getScreenHeight() - content.y - content.height), static_cast<int16_t>(content.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  if (!sdReady_) {
    screen.centeredText(tr(STR_REC_NO_SD), screen.theme().bodyText);
    return;
  }

  fui::ListProps props;
  props.count = static_cast<uint16_t>(entries_.size() + 1);
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
  item.actionValue = static_cast<int16_t>(index);

  if (index == 0) {
    // The entry point to the idle recorder screen, where quality and length
    // are edited before recording.
    item.label = tr(STR_REC_NEW);
    snprintf(self->rowSubtitle_, sizeof(self->rowSubtitle_), "%s · %s",
             SETTINGS.recorderHighQuality ? tr(STR_REC_QUALITY_HIGH_SHORT) : tr(STR_REC_QUALITY_STANDARD_SHORT),
             SETTINGS.recorderNoLimit ? tr(STR_REC_LIMIT_UNLIMITED) : tr(STR_REC_LIMIT_1H));
    item.subtitle = self->rowSubtitle_;
    item.icon = listIconFor(UIIcon::VoiceMemo, 32);
    return;
  }

  const size_t entryIndex = static_cast<size_t>(index - 1);
  if (entryIndex >= self->entries_.size()) return;
  const RecordingEntry& entry = self->entries_[entryIndex];

  recording::formatStartTime(entry.name, self->rowName_, sizeof(self->rowName_));
  item.label = self->rowName_;
  recording::formatDuration(entry.durationMs, self->rowSubtitle_, sizeof(self->rowSubtitle_));
  item.subtitle = self->rowSubtitle_;

  if (strcmp(RECORDING_PLAYER.currentPath(), entry.path) == 0 && RECORDING_PLAYER.isPlaying()) {
    item.value = tr(STR_REC_PLAYING);
  }
  item.icon = listIconFor(UIIcon::VoiceMemo, 32);
}

void RecordingListActivity::activateIndex(const int index) {
  app.clearTapFlash();
  if (index == 0) {
    // Short press opens the recorder idle; hold Confirm starts immediately.
    startRecorder(false);
    return;
  }
  togglePlayback(index - 1);
}

void RecordingListActivity::togglePlayback(const int index) {
  if (index < 0 || index >= static_cast<int>(entries_.size())) return;
  const RecordingEntry& entry = entries_[index];

  if (RECORDING_PLAYER.isPlaying() && strcmp(RECORDING_PLAYER.currentPath(), entry.path) == 0) {
    RECORDING_PLAYER.stop();
    requestUpdate();
    return;
  }

  RECORDING_PLAYER.stop();
#if CROSSPOINT_CAP_MUSIC_PLAYER
  // The memo and the music player share the codec; leaving the music session
  // "playing" over silent output would make its auto-advance fire later.
  if (MUSIC_SESSION.isActive()) MUSIC_SESSION.stop();
#endif
  if (!RECORDING_PLAYER.play(entry.path, kPlaybackVolume)) LOG_ERR("REC", "Cannot play %s", entry.path);
  requestUpdate();
}

void RecordingListActivity::startRecorder(const bool autoStart) {
  RECORDING_PLAYER.stop();
#if CROSSPOINT_CAP_MUSIC_PLAYER
  if (MUSIC_SESSION.isActive()) MUSIC_SESSION.stop();
#endif
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
  const int entryIndex = nav.selected - 1;
  if (entryIndex < 0 || entryIndex >= static_cast<int>(entries_.size())) return;

  const RecordingEntry entry = entries_[entryIndex];
  char title[32];
  recording::formatStartTime(entry.name, title, sizeof(title));
  const std::string path(entry.path);

  auto handler = [this, path](const ActivityResult& result) {
    if (result.isCancelled) return;
    if (strcmp(RECORDING_PLAYER.currentPath(), path.c_str()) == 0) RECORDING_PLAYER.stop();
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
  RECORDING_PLAYER.stop();
  activityManager.goToApps();
}

void RecordingListActivity::drawFooter() {
  const auto labels =
      mappedInput.mapLabels(tr(STR_BACK), RECORDING_PLAYER.isPlaying() ? tr(STR_REC_STOP) : tr(STR_REC_PLAY),
                            tr(STR_REC_HOLD_DELETE), tr(STR_REC_HOLD_RECORD));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

#endif  // CROSSPOINT_CAP_VOICE_MEMO
