#include "HalAudioOutput.h"

#if CROSSPOINT_CAP_SOUND_FEEDBACK || CROSSPOINT_CAP_MUSIC_PLAYER || CROSSPOINT_CAP_VOICE_MEMO

#include <AudioManager.h>
#include <BoardConfig.h>
#include <Logging.h>

#if FREEINK_DEVICE_WAVESHARE_EPAPER_397
#include "Waveshare397Power.h"
#endif

namespace HalAudioOutput {
namespace {

constexpr unsigned long AUDIO_RAIL_SETTLE_MS = 10;

AudioManager audio;
bool initialized = false;
// The music player streams long-form audio through the same codec as the UI
// cues. While it owns the output, feedback cues are dropped: restarting the
// audio task for a beep would cut the song and bump the codec clock.
bool musicOwnsOutput = false;

bool setBoardPower(const bool enabled) {
#if FREEINK_DEVICE_WAVESHARE_EPAPER_397
  return Waveshare397Power::setAudioPower(enabled);
#else
  (void)enabled;
  return true;
#endif
}

bool begin() {
  if (initialized) return true;
  if (!setBoardPower(true)) {
    LOG_ERR("SND", "Failed to enable board audio power");
    return false;
  }
#if FREEINK_DEVICE_WAVESHARE_EPAPER_397
  delay(AUDIO_RAIL_SETTLE_MS);
#endif
  if (!audio.begin()) {
    LOG_ERR("SND", "Failed to initialize audio output");
    setBoardPower(false);
    return false;
  }
  initialized = true;
  return true;
}

}  // namespace

bool playPcm(const ReadCallback read, const SeekCallback seek, const uint32_t sampleRate, const uint8_t channels,
             const uint8_t volume) {
  if (musicOwnsOutput) return false;
  if (!read || !begin()) return false;
  audio.setVolume(volume);
  AudioManager::PcmSource source = {read, seek};
  if (audio.playPcm(source, sampleRate, channels)) return true;
  LOG_ERR("SND", "Failed to play feedback PCM");
  shutdown();
  return false;
}

bool playMusicPcm(const ReadCallback read, const SeekCallback seek, const uint32_t sampleRate,
                  const uint8_t channels, const uint8_t volume) {
  if (!read || !begin()) return false;
  musicOwnsOutput = true;
  audio.setVolume(volume);
  AudioManager::PcmSource source = {read, seek};
  if (audio.playPcm(source, sampleRate, channels)) return true;
  LOG_ERR("SND", "Failed to start music PCM");
  shutdown();
  return false;
}

bool restart(const uint8_t volume) {
  if (!initialized || musicOwnsOutput) return false;
  audio.setVolume(volume);
  return audio.requestRestart();
}

void setVolume(const uint8_t volume) {
  if (initialized) audio.setVolume(volume);
}

bool isPlaying() { return initialized && audio.isPlaying(); }

bool isInitialized() { return initialized; }

void shutdown() {
  musicOwnsOutput = false;
  if (!initialized) return;
  audio.endCapture();
  audio.powerDown();
  if (!setBoardPower(false)) LOG_ERR("SND", "Failed to disable board audio power");
  initialized = false;
}

bool hasMicrophone() { return audio.hasMic(); }

bool beginCapture(const uint32_t sampleRate, const int8_t dinOverride) {
  if (!audio.hasMic()) return false;
  if (!begin()) return false;
  musicOwnsOutput = false;
  if (!audio.beginCapture(sampleRate, dinOverride)) {
    LOG_ERR("SND", "Failed to start microphone capture at %u Hz", static_cast<unsigned>(sampleRate));
    return false;
  }
  return true;
}

size_t readCapture(uint8_t* dst, const size_t len, const uint32_t timeoutMs) {
  return audio.readCapture(dst, len, timeoutMs);
}

size_t writeCapture(const uint8_t* src, const size_t len, const uint32_t timeoutMs) {
  return audio.writeCapture(src, len, timeoutMs);
}

void endCapture() { audio.endCapture(); }

bool isCapturing() { return initialized && audio.isCapturing(); }

bool pokeCodec(const uint8_t reg, const uint8_t value) { return audio.pokeCodec(reg, value); }

uint8_t peekCodec(const uint8_t reg) { return audio.peekCodec(reg); }

}  // namespace HalAudioOutput

#endif
