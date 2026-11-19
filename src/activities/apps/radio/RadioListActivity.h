#pragma once

#if CROSSPOINT_CAP_RADIO

#include "activities/UiListActivity.h"

#include <RadioStations.h>

// Lists the stations imported from /radio and hands playback to
// RadioPlayerActivity. The station list and the live stream live in
// RadioSession, so leaving this screen never interrupts the radio.
class RadioListActivity final : public UiListActivity {
 public:
  RadioListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : UiListActivity("radio", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;

 protected:
  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override;
  bool handleCustomInput() override;

 private:
  static void provideRow(void* ctx, uint16_t index, freeink::ui::ListItem& item);
  void rescan();

  RadioStations::LoadResult loadResult_ = RadioStations::LoadResult::Ok;
  // Backing stores for the row under construction; the list only needs them
  // until the next provider call.
  char rowName_[96] = {};
  char rowSubtitle_[32] = {};
  char rowHeading_[32] = {};
};

#endif  // CROSSPOINT_CAP_RADIO
