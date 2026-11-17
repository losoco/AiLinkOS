#pragma once

#include <cstdint>

#include "StandbyFace.h"
#include "weather/WeatherData.h"

// Standby weather face. Ported from the Waveshare 3.97" reference firmware's
// weather page: amap IP location picks the city, sojson serves the forecast.
//
// Data flow: the face renders whatever is cached on SD immediately, and asks
// StandbyActivity for a silent WiFi session when the snapshot is older than an
// hour. StandbyActivity owns connect/teardown; this face only performs the two
// HTTP GETs inside performNetworkSync().
class WeatherFace final : public StandbyFace {
 public:
  void onEnter() override;
  void onExit() override;
  TickResult tick() override;
  void render(GfxRenderer& renderer, const Rect& viewport) override;
  StrId titleId() const override { return StrId::STR_FACE_WEATHER; }
  uint32_t secondsUntilNextWake() const override { return 3600u; }

  bool wantsNetworkSync() override;
  bool performNetworkSync() override;

 private:
  enum class Error : uint8_t { None, Locate, City, CardFile, Network, Parse };

  WeatherNow data_;
  Error error_ = Error::None;
  // millis() of the last refresh request. Set by wantsNetworkSync() so a face
  // that never gets a WiFi session backs off instead of polling every loop.
  uint32_t lastAttemptMs_ = 0;
  char iconPath_[WeatherNow::kMaxDays][64] = {};

  void load();
  void resolveIcons();
  void renderUnavailable(GfxRenderer& renderer, const Rect& viewport);
  void renderPortrait(GfxRenderer& renderer, const Rect& viewport);
  void renderLandscape(GfxRenderer& renderer, const Rect& viewport);
  void drawIcon(GfxRenderer& renderer, uint8_t dayIndex, int x, int y, int size) const;
};
