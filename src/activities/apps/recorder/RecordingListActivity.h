#pragma once

#if CROSSPOINT_CAP_VOICE_MEMO

#include <RecordingLibrary.h>

#include <vector>

#include "activities/UiListActivity.h"

// Lists the WAV memos under /rec. Confirm plays the selected memo through the
// shared codec; holding Confirm starts a new recording, and holding a direction
// key deletes the selected memo after confirmation.
class RecordingListActivity final : public UiListActivity {
 public:
  RecordingListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : UiListActivity("recordings", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;

 protected:
  // Row 0 is "new recording"; the rest map to entries_ with a -1 offset.
  int listCount() const override { return static_cast<int>(entries_.size()) + 1; }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override;
  bool handleCustomInput() override;
  void onBackButton() override;
  void drawFooter() override;

 private:
  static void provideRow(void* ctx, uint16_t index, freeink::ui::ListItem& item);
  void rescan();
  void startRecorder(bool autoStart);
  void promptDelete();
  void togglePlayback(int index);

  std::vector<RecordingEntry> entries_;
  bool sdReady_ = true;
  bool lastPlaying_ = false;
  // Row backing store; the list only needs these until the next provider call.
  char rowName_[32] = {};
  char rowSubtitle_[32] = {};
};

#endif  // CROSSPOINT_CAP_VOICE_MEMO
