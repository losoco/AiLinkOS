#pragma once

#if CROSSPOINT_CAP_MUSIC_PLAYER

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "AudioSource.h"

// Owns the music playlist and the single playback session.
//
// The AudioManager task pulls PCM through readPcm(); all SD access happens
// there under Storage's lock, so the UI task never blocks on the card during
// playback. Activities only observe/drive this session; they never touch the
// decoder or the audio HAL directly.
class MusicSession {
 public:
  enum class Status : uint8_t { Empty, Ready, Playing, Paused, Stopped, ErrorDecode, ErrorRate };
  enum class LoadResult : uint8_t { Ok, NoSd, NoFolder, NoFiles };

  static MusicSession& get();
  static constexpr const char* kFolder = "/music";
  static constexpr size_t kMaxTracks = 1000;

  LoadResult loadFolder();
  void shutdown();

  size_t trackCount() const { return tracks_.size(); }
  int currentIndex() const { return index_; }
  Status status() const { return status_; }
  bool isPlaying() const { return status_ == Status::Playing; }
  bool isPaused() const { return status_ == Status::Paused; }
  bool isActive() const { return status_ == Status::Playing || status_ == Status::Paused; }
  bool hasError() const { return status_ == Status::ErrorDecode || status_ == Status::ErrorRate; }

  // Display helpers for the list: name without folder/extension, and the
  // container format so a mixed MP3/WAV folder stays readable.
  void trackName(size_t index, char* out, size_t outLen) const;
  AudioFormat trackFormat(size_t index) const;
  // Path of the current track (empty when nothing is selected). Read-only view
  // of the playlist entry, used by the album-cover loader.
  const std::string& currentPath() const;

  bool playIndex(int index);
  // Plays an explicit path list instead of the /music folder. The voice memo
  // list uses this so recordings get the full player UI (pause, volume, skip)
  // without a second decoder/session. A later loadFolder() leaves this mode and
  // stops anything that was playing.
  bool playExternal(std::vector<std::string> paths, int index);
  bool isExternal() const { return external_; }
  bool togglePause();
  bool next();
  bool previous();
  void stop();
  // Called from the app loop (main.cpp) and from the music screens; advances to
  // the next track when the decoder has drained. Cheap when nothing changed, so
  // it stays safe to call on every loop even with no music screen visible.
  void poll();

  uint32_t positionMs() const;
  uint32_t durationMs() const { return durationMs_; }
  uint32_t sampleRate() const { return sampleRate_; }
  uint8_t channels() const { return channels_; }
  uint8_t bitDepth() const { return bitDepth_; }
  AudioFormat format() const { return format_; }

  uint8_t volume() const { return volume_; }
  void setVolume(uint8_t percent);

  static bool isRateSupported(uint32_t sampleRate) { return isAudioRateSupported(sampleRate); }

 private:
  MusicSession() = default;
  static int readThunk(uint8_t* dst, size_t len);
  int readPcm(uint8_t* dst, size_t len);
  bool startPlayback(size_t frameOffset, uint32_t elapsedMs);
  void stopOutput();

  std::vector<std::string> tracks_;
  // True when tracks_ was supplied by playExternal() rather than the /music
  // scan. The player still renders it as an ordinary playlist.
  bool external_ = false;
  // Created per container type on demand; only one backend is alive at a time.
  std::unique_ptr<AudioSource> source_;
  int index_ = -1;
  Status status_ = Status::Empty;
  size_t resumeOffset_ = 0;
  uint32_t resumeMs_ = 0;
  uint32_t durationMs_ = 0;
  uint32_t sampleRate_ = 0;
  uint8_t channels_ = 0;
  uint8_t bitDepth_ = 16;
  AudioFormat format_ = AudioFormat::Mp3;
  uint8_t volume_ = 60;
  size_t pcmByteOffset_ = 0;
  bool ended_ = false;
  bool advancing_ = false;
};

#define MUSIC_SESSION MusicSession::get()

#endif  // CROSSPOINT_CAP_MUSIC_PLAYER
