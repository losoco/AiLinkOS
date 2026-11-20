#pragma once

#include <WavWriter.h>

#include <cstdint>

// One microphone recording: capture start, chunked pull/convert/write, and a
// finalized WAV on the SD card.
//
// The activity loop drives pump() instead of a background task: capture and the
// card both belong to the main task, and a recording only needs a ~50 ms cadence
// to keep the I2S DMA fed. The ES8311 ADC delivers interleaved stereo frames;
// they are averaged to the mono track the memo format uses (half the file size,
// and every ASR backend accepts mono 16 kHz).
class RecordingSession {
 public:
  enum class State : uint8_t { Idle, Recording, Saving, Error };
  enum class Error : uint8_t { None, NoSd, OpenFailed, MicFailed, WriteFailed };

  // maxDurationMs = 0 records until end(). Returns false on failure; error()
  // tells the UI why.
  bool begin(uint16_t sampleRate, uint32_t maxDurationMs);
  // Pulls one chunk from the codec. Returns true when the session ended on its
  // own (the duration limit was reached), so the caller can leave the screen.
  bool pump();
  // Stops capture and backfills the WAV sizes. Safe to call when not recording.
  bool end();

  State state() const { return state_; }
  Error error() const { return error_; }
  bool isRecording() const { return state_ == State::Recording; }
  // Audio-derived duration of the written track, in milliseconds.
  uint32_t elapsedMs() const;
  uint16_t sampleRate() const { return sampleRate_; }
  uint32_t dataBytes() const { return wav_.dataBytes(); }
  const char* path() const { return path_; }

 private:
  static constexpr size_t kFramesPerChunk = 512;
  // The first frames after the ADC starts are a settling transient on this
  // codec; skip them and ramp in over the next few chunks so the memo opens
  // without a click.
  static constexpr uint32_t kStartSkipMs = 120;
  static constexpr uint32_t kRampInMs = 120;
  static constexpr uint32_t kReadTimeoutMs = 60;
  static constexpr uint8_t kMaxZeroReads = 40;  // ~2.4 s without data = dead input

  void fail(Error error);
  void cleanupAfterFailure();
  bool buildPath();

  WavWriter wav_;
  State state_ = State::Idle;
  Error error_ = Error::None;
  char path_[48] = {};

  // Interleaved stereo capture (L,R int16) and the mono chunk written to the
  // card. Both are fixed-size members: no heap, and the audio task never sees
  // them.
  uint8_t capture_[kFramesPerChunk * 4] = {};
  int16_t mono_[kFramesPerChunk] = {};

  uint16_t sampleRate_ = 0;
  uint32_t limitFrames_ = 0;  // 0 = unlimited
  uint32_t frames_ = 0;       // mono frames written
  uint32_t skipFrames_ = 0;
  uint32_t rampFrames_ = 0;
  uint32_t skipLeft_ = 0;
  uint32_t rampPos_ = 0;
  uint8_t zeroReads_ = 0;
};
