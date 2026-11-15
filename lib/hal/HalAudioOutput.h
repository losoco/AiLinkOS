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

}  // namespace HalAudioOutput
