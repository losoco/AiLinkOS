#pragma once

#if CROSSPOINT_CAP_SCREEN_MIRROR

#include "activities/Activity.h"

// Control page for the LAN screen mirror. The service itself is global: leaving
// this page keeps the mirror alive so the user can go back to reading.
class ScreenMirrorActivity final : public Activity {
 public:
  ScreenMirrorActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("screen-mirror", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override;
  bool skipLoopDelay() override;

 private:
  int lastState_ = -1;
  uint8_t lastClients_ = 0;
};

#endif  // CROSSPOINT_CAP_SCREEN_MIRROR
