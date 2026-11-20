#pragma once

#include <HalAudioOutput.h>
#include <HalStorage.h>

#include <cstddef>
#include <cstdint>

// Plays one /rec WAV through the shared codec while the list screen stays open.
//
// HalAudioOutput's PCM source is a pair of C callbacks, so the session is a
// singleton: the read callback needs a fixed function to find the open file
// again. Unlike the music player this is short-form playback with no playlist,
// pause, or seek — stopping just releases the card and the audio rail.
class RecordingPlayer {
 public:
  static RecordingPlayer& get();

  bool play(const char* path, uint8_t volume);
  void stop();
  // Closes the file once the audio task drains; call from the activity loop.
  void poll();

  bool isPlaying() const { return ownsOutput_ && HalAudioOutput::isPlaying(); }
  const char* currentPath() const { return path_; }

 private:
  RecordingPlayer() = default;
  static int readThunk(uint8_t* dst, size_t len);
  static bool seekThunk(size_t pos);
  int read(uint8_t* dst, size_t len);

  HalFile file_;
  uint32_t dataBytes_ = 0;
  uint32_t pos_ = 0;
  uint16_t sampleRate_ = 0;
  uint8_t channels_ = 1;
  bool ownsOutput_ = false;
  char path_[48] = {};
};

#define RECORDING_PLAYER RecordingPlayer::get()
