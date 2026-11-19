#pragma once

#if CROSSPOINT_CAP_RADIO

#include "activities/Activity.h"
#include "RadioSession.h"

// Live radio page: station name, stream state and transport controls.
//
// There is no seek, duration or track list here: the stream is live and the
// station list is one Back press away. Playback keeps running in the background
// (RadioSession owns it) when this page closes.
class RadioPlayerActivity final : public Activity {
 public:
  RadioPlayerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("radio-player", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  void onExit() override;
  bool preventAutoSleep() override;
  void prepareForSleep() override;

 private:
  void changeVolume(int delta);
  void drawVolumeOverlay(const Rect& content, uint8_t volume);

  RadioSession::Status lastStatus_ = RadioSession::Status::Empty;
  int lastIndex_ = -1;
  bool fullDirty_ = true;
  bool stationDirty_ = false;
  bool volumeOverlay_ = false;
  uint32_t overlayDeadlineMs_ = 0;
  bool volumeDirty_ = false;
  GfxRenderer::Orientation previousOrientation_ = GfxRenderer::Orientation::Portrait;
};

#endif  // CROSSPOINT_CAP_RADIO
