#pragma once

#if CROSSPOINT_CAP_VOICE_MEMO

#include <RecordingLibrary.h>

#include <vector>

#include "activities/UiListActivity.h"

// Lists the WAV memos under /rec. Confirm hands the selected memo to the music
// player (play/pause, volume and skip), holding Confirm starts a new recording,
// and holding a direction key deletes the selected memo after confirmation.
class RecordingListActivity final : public UiListActivity {
 public:
  RecordingListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : UiListActivity("recordings", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;

 protected:
  int listCount() const override { return static_cast<int>(entries_.size()); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override;
  bool handleCustomInput() override;
  void onBackButton() override;
  void drawFooter() override;

 private:
  static void provideRow(void* ctx, uint16_t index, freeink::ui::ListItem& item);
  void rescan();
  void openPlayer(int index);
  void startRecorder(bool autoStart);
  void promptDelete();

  std::vector<RecordingEntry> entries_;
  bool sdReady_ = true;
  // Row backing store; the list only needs these until the next provider call.
  char rowName_[32] = {};
  char rowSubtitle_[32] = {};
};

#endif  // CROSSPOINT_CAP_VOICE_MEMO
