#pragma once

#if CROSSPOINT_CAP_MUSIC_PLAYER

#include <cstddef>
#include <cstdint>
#include <string>

#include "AudioSource.h"
#include "HalStorage.h"

// Streams PCM from a RIFF/WAVE file without a third-party decoder.
//
// Accepted: uncompressed PCM (format tag 1, or WAVE_FORMAT_EXTENSIBLE whose
// sub-format is PCM) at 16 bits per sample, 1-2 channels. Everything else
// (ADPCM, float, 8/24/32-bit) fails open() so the player shows its error page
// instead of playing noise.
class WavSource final : public AudioSource {
 public:
  WavSource() = default;
  ~WavSource() override;

  bool open(const std::string& path) override;
  bool openAt(const std::string& path, size_t offset, uint32_t elapsedMs) override;
  void close() override;
  bool decodeFrame() override;

  const int16_t* pcm() const override { return pcm_; }
  int pcmSampleCount() const override { return samples_ * channels_; }
  uint32_t sampleRate() const override { return sampleRate_; }
  uint8_t channels() const override { return channels_; }
  uint8_t bitDepth() const override { return 16; }
  AudioFormat format() const override { return AudioFormat::Wav; }
  uint32_t durationMs() const override { return durationMs_; }
  uint32_t elapsedMs() const override { return elapsedMs_; }
  size_t nextFrameOffset() const override { return dataPos_; }
  bool ended() const override { return ended_; }
  bool hadError() const override { return error_; }

 private:
  // 1024 bytes = 512 mono or 256 stereo frames, about 23 ms at 44.1 kHz.
  static constexpr size_t kChunkBytes = 1024;

  bool readAt(size_t pos, void* dst, size_t len);
  bool parseHeader();
  bool reopen(const std::string& path);

  HalFile file_;
  int16_t pcm_[kChunkBytes / 2] = {};
  int samples_ = 0;
  uint32_t fileSize_ = 0;
  uint32_t dataStart_ = 0;
  uint32_t dataBytes_ = 0;
  uint32_t dataPos_ = 0;
  uint32_t sampleRate_ = 0;
  uint32_t byteRate_ = 0;
  uint32_t durationMs_ = 0;
  uint32_t elapsedMs_ = 0;
  uint16_t blockAlign_ = 0;
  uint8_t channels_ = 0;
  bool ended_ = false;
  bool error_ = false;
};

#endif  // CROSSPOINT_CAP_MUSIC_PLAYER
