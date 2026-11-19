#pragma once

#if CROSSPOINT_CAP_RADIO

#include <RadioStations.h>
#include <RadioStream.h>

#include <atomic>
#include <cstdint>

class GfxRenderer;

// Owns the imported station list and the single radio stream.
//
// The audio task pulls PCM through readPcm(), so the socket and the decoder
// live on that task exactly like the local music player. Activities only read
// the state and issue play/stop/volume commands.
//
// One live stream at a time: starting radio stops the music player and vice
// versa, because both drive the same ES8311/I2S path.
class RadioSession {
 public:
  enum class Status : uint8_t { Empty, Ready, Connecting, Playing, Paused, Stopped, Error };

  static RadioSession& get();

  RadioStations::LoadResult loadStations();
  void shutdown();

  size_t stationCount() const { return stations_.count(); }
  const RadioStation* stationAt(const size_t index) const {
    return index < stations_.count() ? &stations_.at(index) : nullptr;
  }
  int currentIndex() const { return index_; }
  Status status() const { return status_; }
  bool isPlaying() const { return status_ == Status::Playing; }
  bool isPaused() const { return status_ == Status::Paused; }
  bool isActive() const { return status_ == Status::Connecting || isPlaying() || isPaused(); }
  const char* errorText() const { return error_; }

  bool playIndex(int index);
  bool togglePause();
  bool next();
  bool previous();
  void stop();
  // Called from the app loop: turns a dead stream into an error state once the
  // audio task has drained, so the UI stops claiming to play.
  void poll();

  uint8_t volume() const { return volume_; }
  void setVolume(uint8_t percent);

  // Wi-Fi bring-up needs the renderer (NetworkStartup releases render memory
  // before the driver allocates); the list/player attach it on entry.
  void attachRenderer(GfxRenderer* renderer) { renderer_ = renderer; }

 private:
  RadioSession() = default;
  static int readThunk(uint8_t* dst, size_t len);
  int readPcm(uint8_t* dst, size_t len);
  bool ensureWifi();
  bool startStream();
  void serviceConnect();
  void stopOutput();
  void stopMusicPlayer();

  RadioStations stations_;
  RadioStream stream_;
  GfxRenderer* renderer_ = nullptr;
  int index_ = -1;
  Status status_ = Status::Empty;
  std::atomic<bool> streamFailed_{false};
  bool pendingConnect_ = false;
  uint32_t connectAtMs_ = 0;
  char error_[32] = {};
  uint8_t volume_ = 60;
};

// Singleton accessor, matching MUSIC_SESSION / SETTINGS / GUI style.
#define RADIO_SESSION RadioSession::get()

#endif  // CROSSPOINT_CAP_RADIO
