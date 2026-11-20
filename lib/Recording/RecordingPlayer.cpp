#include "RecordingPlayer.h"

#include <Logging.h>

#include <cstdio>
#include <cstring>
#include <utility>

RecordingPlayer& RecordingPlayer::get() {
  static RecordingPlayer instance;
  return instance;
}

bool RecordingPlayer::play(const char* path, const uint8_t volume) {
  stop();
  if (path == nullptr || path[0] == '\0') return false;

  HalFile file;
  if (!Storage.openFileForRead("REC", path, file)) return false;

  uint8_t header[44];
  if (file.read(header, sizeof(header)) != static_cast<int>(sizeof(header)) || memcmp(header, "RIFF", 4) != 0 ||
      memcmp(header + 8, "WAVE", 4) != 0) {
    LOG_ERR("REC", "Not a WAV recording: %s", path);
    return false;
  }
  uint16_t channels = 0;
  uint32_t sampleRate = 0;
  uint32_t dataBytes = 0;
  memcpy(&channels, header + 22, sizeof(channels));
  memcpy(&sampleRate, header + 24, sizeof(sampleRate));
  memcpy(&dataBytes, header + 40, sizeof(dataBytes));
  if ((channels != 1 && channels != 2) || sampleRate < 8000 || sampleRate > 48000 || dataBytes == 0) {
    LOG_ERR("REC", "Unsupported WAV format: %s", path);
    return false;
  }

  file_ = std::move(file);
  dataBytes_ = dataBytes;
  pos_ = 0;
  sampleRate_ = static_cast<uint16_t>(sampleRate);
  channels_ = static_cast<uint8_t>(channels);
  snprintf(path_, sizeof(path_), "%s", path);

  // The memo shares the codec with music and the microphone; release whatever
  // holds it before starting the PCM stream.
  HalAudioOutput::shutdown();
  if (!HalAudioOutput::playMusicPcm(readThunk, seekThunk, sampleRate_, channels_, volume)) {
    LOG_ERR("REC", "Cannot start playback: %s", path);
    file_.close();
    path_[0] = '\0';
    return false;
  }
  ownsOutput_ = true;
  return true;
}

void RecordingPlayer::stop() {
  if (ownsOutput_) {
    // Stops the audio task before the file handle it reads from goes away.
    HalAudioOutput::shutdown();
    ownsOutput_ = false;
  }
  if (file_.isOpen()) file_.close();
  dataBytes_ = 0;
  pos_ = 0;
  path_[0] = '\0';
}

void RecordingPlayer::poll() {
  if (ownsOutput_ && !HalAudioOutput::isPlaying()) stop();
}

int RecordingPlayer::readThunk(uint8_t* dst, const size_t len) { return get().read(dst, len); }

bool RecordingPlayer::seekThunk(size_t) { return false; }

int RecordingPlayer::read(uint8_t* dst, const size_t len) {
  if (dst == nullptr || len == 0 || !file_.isOpen() || pos_ >= dataBytes_) return 0;
  const size_t remaining = dataBytes_ - pos_;
  const size_t want = len < remaining ? len : remaining;
  const int got = file_.read(dst, want);
  if (got <= 0) return 0;
  pos_ += static_cast<uint32_t>(got);
  return got;
}
