#pragma once

#if CROSSPOINT_CAP_MUSIC_PLAYER

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

enum class AudioFormat : uint8_t { Mp3, Wav };

// Decoder contract shared by every backend the player can open. MusicSession
// owns exactly one at a time and streams its PCM through the audio task.
//
// Everything below is reported as the *file* declares it: the session forwards
// sampleRate() to AudioManager, which programs the ES8311 clock for that rate
// (256 x fs), so no backend may silently resample.
class AudioSource {
 public:
  virtual ~AudioSource() = default;

  // Opens `path` and reads its stream parameters. False for missing or
  // undecodable files (the caller shows its error state).
  virtual bool open(const std::string& path) = 0;
  // Reopens at a previously returned nextFrameOffset() (pause/resume).
  virtual bool openAt(const std::string& path, size_t offset, uint32_t elapsedMs) = 0;
  virtual void close() = 0;

  // Decodes the next chunk into pcm(); false at end of stream or on error.
  // Check ended()/hadError() to tell a clean end from a corrupt file.
  virtual bool decodeFrame() = 0;

  virtual const int16_t* pcm() const = 0;
  // Interleaved samples (frames * channels).
  virtual int pcmSampleCount() const = 0;
  virtual uint32_t sampleRate() const = 0;
  virtual uint8_t channels() const = 0;
  virtual uint8_t bitDepth() const = 0;
  virtual AudioFormat format() const = 0;
  virtual uint32_t durationMs() const = 0;
  virtual uint32_t elapsedMs() const = 0;
  // File offset of the next chunk, for pause/resume.
  virtual size_t nextFrameOffset() const = 0;
  virtual bool ended() const = 0;
  virtual bool hadError() const = 0;
};

// AudioManager streams 16-bit PCM between 8 and 48 kHz.
inline bool isAudioRateSupported(const uint32_t sampleRate) {
  return sampleRate >= 8000 && sampleRate <= 48000;
}

inline bool hasExtensionIgnoreCase(std::string_view path, std::string_view extension) {
  if (path.size() < extension.size()) return false;
  const size_t offset = path.size() - extension.size();
  for (size_t i = 0; i < extension.size(); ++i) {
    char c = path[offset + i];
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    if (c != extension[i]) return false;
  }
  return true;
}

inline bool isMp3Path(std::string_view path) { return hasExtensionIgnoreCase(path, ".mp3"); }
inline bool isWavPath(std::string_view path) { return hasExtensionIgnoreCase(path, ".wav"); }
inline bool isSupportedAudioPath(std::string_view path) { return isMp3Path(path) || isWavPath(path); }

inline AudioFormat audioFormatForPath(std::string_view path) {
  return isWavPath(path) ? AudioFormat::Wav : AudioFormat::Mp3;
}

#endif  // CROSSPOINT_CAP_MUSIC_PLAYER
