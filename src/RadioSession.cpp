#include "RadioSession.h"

#if CROSSPOINT_CAP_RADIO

#include <GfxRenderer.h>
#include <HalAudioOutput.h>
#include <Logging.h>
#include <NetworkStartup.h>
#include <WiFi.h>
#include <WifiCredentialStore.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <optional>

#if CROSSPOINT_CAP_MUSIC_PLAYER
#include <MusicSession.h>
#define HAS_MUSIC_PLAYER 1
#else
#define HAS_MUSIC_PLAYER 0
#endif

namespace {
// Give the newly opened player one frame to paint "connecting" before the
// handshake blocks the app loop.
constexpr uint32_t kConnectGraceMs = 400;
constexpr uint32_t kWifiJoinTimeoutMs = 15000;
}  // namespace

RadioSession& RadioSession::get() {
  static RadioSession instance;
  return instance;
}

RadioStations::LoadResult RadioSession::loadStations() {
  stop();
  return stations_.load();
}

void RadioSession::stopMusicPlayer() {
#if HAS_MUSIC_PLAYER
  // Both use the same codec; whoever starts last owns it.
  if (MUSIC_SESSION.isActive()) MUSIC_SESSION.stop();
#endif
}

void RadioSession::stopOutput() { HalAudioOutput::shutdown(); }

bool RadioSession::ensureWifi() {
  if (WiFi.status() == WL_CONNECTED) return true;
  if (!renderer_) {
    std::snprintf(error_, sizeof(error_), "%s", "no wifi");
    return false;
  }
  // Bringing the STA up installs the lwIP netif; resolving a host before that
  // asserts inside lwIP (the stack has no message queue yet).
  if (WiFi.getMode() == WIFI_MODE_NULL && !NetworkStartup::setMode(*renderer_, WIFI_STA)) {
    std::snprintf(error_, sizeof(error_), "%s", "wifi start failed");
    return false;
  }

  if (WIFI_STORE.getCredentialCount() == 0) WIFI_STORE.loadFromFile();
  const std::string lastSsid = WIFI_STORE.getLastConnectedSsid();
  const auto credential = lastSsid.empty() ? std::nullopt : WIFI_STORE.findCredential(lastSsid);
  if (!credential) {
    std::snprintf(error_, sizeof(error_), "%s", "no saved wifi");
    return false;
  }

  WiFi.begin(credential->ssid.c_str(), credential->password.c_str());
  LOG_INF("RAD", "connecting WiFi %s", credential->ssid.c_str());
  const uint32_t deadline = millis() + kWifiJoinTimeoutMs;
  while (millis() < deadline && WiFi.status() != WL_CONNECTED) delay(100);
  if (WiFi.status() != WL_CONNECTED) {
    std::snprintf(error_, sizeof(error_), "%s", "wifi timeout");
    return false;
  }
  WIFI_STORE.setLastConnectedSsid(std::string(WiFi.SSID().c_str()));
  LOG_INF("RAD", "WiFi connected %s", WiFi.localIP().toString().c_str());
  return true;
}

bool RadioSession::startStream() {
  const RadioStation* station = stationAt(static_cast<size_t>(index_));
  if (!station) {
    std::snprintf(error_, sizeof(error_), "%s", "no station");
    return false;
  }

  const bool opened = stream_.open(station->url);
  if (!opened) {
    std::snprintf(error_, sizeof(error_), "%s", stream_.errorText());
    LOG_ERR("RAD", "Open failed for %s: %s", station->name, stream_.errorText());
    return false;
  }
  streamFailed_.store(false);
  if (!HalAudioOutput::playMusicPcm(readThunk, nullptr, stream_.sampleRate(), stream_.channels(), volume_)) {
    std::snprintf(error_, sizeof(error_), "%s", "audio busy");
    stream_.close();
    return false;
  }
  LOG_INF("RAD", "Playing %s (%u Hz %u ch)", station->name, static_cast<unsigned>(stream_.sampleRate()),
          static_cast<unsigned>(stream_.channels()));
  return true;
}

void RadioSession::serviceConnect() {
  pendingConnect_ = false;
  if (!ensureWifi() || !startStream()) {
    stopOutput();
    stream_.close();
    status_ = Status::Error;
    LOG_ERR("RAD", "Connect failed: %s", error_);
    return;
  }
  status_ = Status::Playing;
}

bool RadioSession::playIndex(const int index) {
  if (index < 0 || static_cast<size_t>(index) >= stations_.count()) return false;
  stopMusicPlayer();
  stopOutput();
  stream_.close();
  index_ = index;
  error_[0] = '\0';
  status_ = Status::Connecting;
  // The handshake blocks for seconds, so it runs from poll() after the page has
  // been painted instead of inside the input handler.
  pendingConnect_ = true;
  connectAtMs_ = millis() + kConnectGraceMs;
  return true;
}

bool RadioSession::togglePause() {
  if (status_ == Status::Playing) {
    stopOutput();
    stream_.close();
    status_ = Status::Paused;
    return true;
  }
  if (status_ == Status::Paused || status_ == Status::Error) {
    status_ = Status::Connecting;
    pendingConnect_ = true;
    connectAtMs_ = millis() + kConnectGraceMs;
    return true;
  }
  return false;
}

bool RadioSession::next() {
  if (stations_.empty()) return false;
  const int nextIndex = (index_ + 1) % static_cast<int>(stations_.count());
  return playIndex(nextIndex);
}

bool RadioSession::previous() {
  if (stations_.empty()) return false;
  const int prevIndex = (index_ - 1 + static_cast<int>(stations_.count())) % static_cast<int>(stations_.count());
  return playIndex(prevIndex);
}

void RadioSession::stop() {
  pendingConnect_ = false;
  stopOutput();
  stream_.close();
  streamFailed_.store(false);
  status_ = stations_.empty() ? Status::Empty : Status::Stopped;
}

void RadioSession::poll() {
  if (pendingConnect_ && static_cast<int32_t>(millis() - connectAtMs_) >= 0) {
    serviceConnect();
    return;
  }
  if (status_ != Status::Playing) return;
  if (streamFailed_.load() && !HalAudioOutput::isPlaying()) {
    // The audio task drained after the socket died. Keep the station selected so
    // Confirm can retry it.
    stopOutput();
    stream_.close();
    status_ = Status::Error;
    if (error_[0] == '\0') std::snprintf(error_, sizeof(error_), "%s", "stream lost");
    LOG_ERR("RAD", "Stream stopped: %s", error_);
  }
}

void RadioSession::shutdown() {
  stop();
  stations_.clear();
  index_ = -1;
  status_ = Status::Empty;
  renderer_ = nullptr;
}

void RadioSession::setVolume(const uint8_t percent) {
  volume_ = percent > 100 ? 100 : percent;
  if (HalAudioOutput::isInitialized()) HalAudioOutput::setVolume(volume_);
}

int RadioSession::readThunk(uint8_t* dst, const size_t len) { return get().readPcm(dst, len); }

int RadioSession::readPcm(uint8_t* dst, const size_t len) {
  const int n = stream_.readPcm(dst, len);
  if (n <= 0) {
    // A dead socket ends the audio task; poll() turns this into the error state.
    streamFailed_.store(true);
    return 0;
  }
  return n;
}

#endif  // CROSSPOINT_CAP_RADIO
