#pragma once

#if CROSSPOINT_CAP_MUSIC_PLAYER

#include <cstddef>
#include <cstdint>
#include <string>

#include "AudioSource.h"
#include "HalStorage.h"
#include "minimp3.h"

// Decodes an MP3 file from the SD card frame by frame.
//
// Frame-aligned iteration (rather than minimp3's streaming helper) is what lets
// the player pause and resume without rescanning: the position of the next
// frame header is always known and can be reopened exactly.
class Mp3Source final : public AudioSource {
 public:
  Mp3Source();
  ~Mp3Source() override;
  Mp3Source(const Mp3Source&) = delete;
  Mp3Source& operator=(const Mp3Source&) = delete;

  // Opens `path`, skips ID3v2, locates the first valid frame and reads its
  // stream parameters. Returns false for missing/undecodable files.
  bool open(const std::string& path) override;
  // Reopens at a previously returned frame offset (pause/resume).
  bool openAt(const std::string& path, size_t frameOffset, uint32_t elapsedMs) override;
  // Closes the file and releases decoder state.
  void close() override;

  // Decodes the next frame into `pcm()`; false at end of stream or on error.
  // Check ended()/hadError() to tell a clean end from a corrupt or truncated
  // file (the player shows an error page instead of auto-advancing).
  bool decodeFrame() override;

  const int16_t* pcm() const override { return pcm_; }
  // Interleaved samples (frames * channels).
  int pcmSampleCount() const override { return pcmSamples_ * channels_; }
  int frameSampleCount() const { return pcmSamples_; }

  uint32_t sampleRate() const override { return sampleRate_; }
  uint8_t channels() const override { return channels_; }
  // minimp3 always hands back 16-bit PCM.
  uint8_t bitDepth() const override { return 16; }
  AudioFormat format() const override { return AudioFormat::Mp3; }
  uint32_t durationMs() const override { return durationMs_; }
  uint32_t elapsedMs() const override { return elapsedMs_; }
  size_t nextFrameOffset() const override { return framePos_; }
  bool ended() const override { return ended_; }
  bool hadError() const override { return error_; }

 private:
  struct FrameHeader {
    uint32_t sampleRate = 0;
    uint16_t frameBytes = 0;
    uint16_t samples = 0;  // per channel
    uint8_t channels = 0;
    uint8_t bitrateKbps = 0;
    bool mpeg1 = false;
  };

  bool readAt(size_t pos, void* dst, size_t len);
  bool parseHeader(const uint8_t* h, FrameHeader& out) const;
  bool findNextFrame(size_t from, size_t& found, FrameHeader& out);
  bool readFrame(const FrameHeader& h);
  void estimateDuration(size_t audioStart, const FrameHeader& first);

  HalFile file_;
  mp3dec_t decoder_{};
  int16_t pcm_[MINIMP3_MAX_SAMPLES_PER_FRAME] = {};
  uint8_t frameBuf_[2304] = {};  // MAX_FREE_FORMAT_FRAME_SIZE
  int pcmSamples_ = 0;
  size_t framePos_ = 0;
  size_t fileSize_ = 0;
  uint32_t sampleRate_ = 0;
  uint32_t durationMs_ = 0;
  uint32_t elapsedMs_ = 0;
  uint8_t channels_ = 0;
  bool ended_ = false;
  bool error_ = false;
};

#endif  // CROSSPOINT_CAP_MUSIC_PLAYER
