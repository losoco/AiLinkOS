#pragma once

#include <cstddef>
#include <cstdint>

namespace HalAudioOutput {

using ReadCallback = int (*)(uint8_t* dst, size_t length);
using SeekCallback = bool (*)(size_t position);

// Short UI cue. Returns false (without touching the output) while the music
// player owns the shared codec, so a button press cannot cut the song short.
bool playPcm(ReadCallback read, SeekCallback seek, uint32_t sampleRate, uint8_t channels, uint8_t volume);
// Long, exclusive playback (music player). Takes ownership of the shared codec
// until shutdown(); UI cues are dropped in the meantime.
bool playMusicPcm(ReadCallback read, SeekCallback seek, uint32_t sampleRate, uint8_t channels, uint8_t volume);
bool restart(uint8_t volume);
void setVolume(uint8_t volume);
void shutdown();
// True while the audio task is actively streaming (music player auto-advance).
bool isPlaying();
bool isInitialized();

// Microphone capture through the same codec (boards with an I2S codec mic).
// Playback and capture share the I2S peripheral: beginCapture() stops any
// output, and endCapture() leaves the codec ready for the next play call.
bool hasMicrophone();
bool beginCapture(uint32_t sampleRate, int8_t dinOverride = -1);
// Blocking read of interleaved 16-bit stereo frames; 0 on timeout.
size_t readCapture(uint8_t* dst, size_t len, uint32_t timeoutMs);
// Diagnostics: write interleaved 16-bit stereo to the capture-time TX channel.
size_t writeCapture(const uint8_t* src, size_t len, uint32_t timeoutMs);
void endCapture();
bool isCapturing();

// Diagnostics: raw codec register access, used by the serial debug commands to
// poke a codec without a reflash.
bool pokeCodec(uint8_t reg, uint8_t value);
uint8_t peekCodec(uint8_t reg);

}  // namespace HalAudioOutput
