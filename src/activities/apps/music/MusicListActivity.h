#pragma once

#include "activities/UiListActivity.h"

#if CROSSPOINT_CAP_MUSIC_PLAYER

#include <MusicSession.h>

// Lists the MP3/WAV files in /music and hands playback to MusicPlayerActivity.
// The playlist lives in MusicSession, so this screen only renders it and never
// owns the decoder or the audio output.
class MusicListActivity final : public UiListActivity {
 public:
  MusicListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : UiListActivity("music", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void onExit() override;

 protected:
  int listCount() const override { return static_cast<int>(MUSIC_SESSION.trackCount()); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override;
  bool handleCustomInput() override;
  void onBackButton() override;
  void drawFooter() override;

 private:
  static void provideRow(void* ctx, uint16_t index, freeink::ui::ListItem& item);
  void rescan();

  MusicSession::LoadResult loadResult_ = MusicSession::LoadResult::Ok;
  int lastIndex_ = -1;
  int lastStatus_ = -1;
  // Orientation to restore when the list closes; the list itself follows the
  // player's remembered orientation so rotating only in the player is coherent.
  GfxRenderer::Orientation previousOrientation_ = GfxRenderer::Orientation::Portrait;
  // Backing store for the label of the row being provided; list.h guarantees
  // the pointers only need to survive until the next provider call.
  char rowName_[192] = {};
};

#endif  // CROSSPOINT_CAP_MUSIC_PLAYER
