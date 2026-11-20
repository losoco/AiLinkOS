#pragma once

#include <RecordingSession.h>
#include <SloppyDigits.h>

#include "activities/Activity.h"

#if CROSSPOINT_CAP_VOICE_MEMO

struct Rect;

// Voice memo recorder screen: big elapsed clock, the two quality/duration
// switches, and one button to start/stop. The session owns capture and the WAV
// file; this activity only pumps it, draws it, and translates gestures.
//
// Entered from the recording list. autoStart skips the idle screen so a long
// Confirm press on the list lands directly in a running recording.
class RecorderActivity final : public Activity {
 public:
  RecorderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool autoStart = false)
      : Activity("recorder", renderer, mappedInput), autoStart_(autoStart) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override;
  void prepareForSleep() override;
  // The main loop's 10 ms idle delay (and the 50 ms idle sleep) is enough to
  // starve the I2S DMA and drop roughly a quarter of the audio.
  bool skipLoopDelay() override { return session_.isRecording(); }

 private:
  void startRecording();
  const char* statusText() const;
  // Sloppy geometric digits plus hand-placed colons (the sloppy renderer only
  // draws digits, so MM:SS punctuation is ours).
  void drawTimer(const Rect& bounds, uint32_t seconds) const;
  void drawOptions(const Rect& bounds) const;

  RecordingSession session_;
  const bool autoStart_;
  int lastShownSecond_ = -1;
  sloppy::Style timerStyle_{};
  sloppy::Seeds timerSeeds_{};
};

#endif  // CROSSPOINT_CAP_VOICE_MEMO
