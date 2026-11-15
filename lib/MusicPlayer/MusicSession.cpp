#include "MusicSession.h"

#if CROSSPOINT_CAP_MUSIC_PLAYER

#include <strings.h>

#include <algorithm>
#include <cstring>
#include <string_view>

#include "FsHelpers.h"
#include "HalAudioOutput.h"
#include "Logging.h"
#include "Memory.h"
#include "Mp3Source.h"
#include "Utf8.h"
#include "WavSource.h"

namespace {

constexpr size_t kNameBufferBytes = 192;

void copyDisplayName(const char* rawName, char* out, const size_t outLen) {
  if (!out || outLen == 0) return;
  out[0] = '\0';
  if (!rawName) return;

  size_t nameLen = strlen(rawName);
  if (nameLen > 4 &&
      (strcasecmp(rawName + nameLen - 4, ".mp3") == 0 || strcasecmp(rawName + nameLen - 4, ".wav") == 0)) {
    nameLen -= 4;
  }

  const size_t maxBytes = outLen - 1;
  bool truncated = nameLen > maxBytes;
  size_t copyLen = truncated ? maxBytes : nameLen;
  if (truncated) {
    // Never cut through a UTF-8 continuation byte.
    while (copyLen > 0 && (static_cast<uint8_t>(rawName[copyLen]) & 0xC0) == 0x80) --copyLen;
  }
  memcpy(out, rawName, copyLen);
  size_t end = copyLen;
  if (truncated && end >= 3) {
    out[end - 3] = '.';
    out[end - 2] = '.';
    out[end - 1] = '.';
  }
  out[end] = '\0';
  utf8ComposeNfcInPlace(out);
}

}  // namespace

MusicSession& MusicSession::get() {
  static MusicSession instance;
  return instance;
}

MusicSession::LoadResult MusicSession::loadFolder() {
  status_ = Status::Empty;
  ended_ = true;
  stopOutput();
  source_.reset();
  index_ = -1;
  resumeOffset_ = 0;
  resumeMs_ = 0;
  durationMs_ = 0;
  sampleRate_ = 0;
  channels_ = 0;
  bitDepth_ = 16;
  tracks_.clear();
  status_ = Status::Empty;

  if (!Storage.ready()) return LoadResult::NoSd;

  {
    HalStorage::StorageLock lock;
    HalFile dir = Storage.open(kFolder);
    if (!dir.isOpen() || !dir.isDirectory()) return LoadResult::NoFolder;

    char name[256];
    for (HalFile entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
      if (entry.isDirectory()) continue;
      if (entry.getName(name, sizeof(name)) == 0) continue;
      if (name[0] == '\0' || name[0] == '.') continue;
      if (!isSupportedAudioPath(std::string_view(name))) continue;
      if (tracks_.size() >= kMaxTracks) break;

      std::string path(kFolder);
      path += '/';
      path += name;
      tracks_.push_back(std::move(path));
    }
  }

  FsHelpers::sortFileList(tracks_);
  status_ = tracks_.empty() ? Status::Empty : Status::Ready;
  return tracks_.empty() ? LoadResult::NoFiles : LoadResult::Ok;
}

void MusicSession::trackName(const size_t index, char* out, const size_t outLen) const {
  if (!out || outLen == 0) return;
  out[0] = '\0';
  if (index >= tracks_.size()) return;

  const char* path = tracks_[index].c_str();
  const char* slash = strrchr(path, '/');
  copyDisplayName(slash ? slash + 1 : path, out, std::min(outLen, kNameBufferBytes));
}

AudioFormat MusicSession::trackFormat(const size_t index) const {
  if (index >= tracks_.size()) return AudioFormat::Mp3;
  return audioFormatForPath(std::string_view(tracks_[index]));
}

const std::string& MusicSession::currentPath() const {
  // Empty-string SSO: no allocation on the no-track path or on any call.
  static const std::string kEmpty;
  if (index_ < 0 || static_cast<size_t>(index_) >= tracks_.size()) return kEmpty;
  return tracks_[static_cast<size_t>(index_)];
}

int MusicSession::readThunk(uint8_t* dst, const size_t len) { return get().readPcm(dst, len); }

int MusicSession::readPcm(uint8_t* dst, const size_t len) {
  if (status_ != Status::Playing || !source_ || ended_) return 0;

  // Runs in the AudioManager task; the recursive storage lock keeps the UI task
  // from interleaving SD transactions while a frame is being read.
  HalStorage::StorageLock lock;
  size_t written = 0;
  while (written < len) {
    const size_t frameBytes = static_cast<size_t>(source_->pcmSampleCount()) * sizeof(int16_t);
    if (pcmByteOffset_ < frameBytes) {
      const uint8_t* src = reinterpret_cast<const uint8_t*>(source_->pcm()) + pcmByteOffset_;
      const size_t chunk = std::min(frameBytes - pcmByteOffset_, len - written);
      memcpy(dst + written, src, chunk);
      written += chunk;
      pcmByteOffset_ += chunk;
      continue;
    }
    if (!source_->decodeFrame()) {
      ended_ = true;
      // A truncated or corrupt file stops the output and shows the error page;
      // only a clean end of stream lets poll() advance to the next track.
      if (source_->hadError()) {
        LOG_ERR("MUS", "Decode failed in %s", tracks_[index_].c_str());
        status_ = Status::ErrorDecode;
      }
      break;
    }
    pcmByteOffset_ = 0;
  }
  return static_cast<int>(written);
}

bool MusicSession::startPlayback(const size_t frameOffset, const uint32_t elapsedMs) {
  if (index_ < 0 || index_ >= static_cast<int>(tracks_.size())) return false;

  const AudioFormat format = trackFormat(static_cast<size_t>(index_));
  if (!source_ || source_->format() != format) {
    source_.reset();
    source_ = format == AudioFormat::Wav ? std::unique_ptr<AudioSource>(makeUniqueNoThrow<WavSource>())
                                         : std::unique_ptr<AudioSource>(makeUniqueNoThrow<Mp3Source>());
    if (!source_) {
      LOG_ERR("MUS", "OOM allocating audio decoder");
      status_ = Status::ErrorDecode;
      return false;
    }
  }

  const bool opened =
      frameOffset == 0 ? source_->open(tracks_[index_]) : source_->openAt(tracks_[index_], frameOffset, elapsedMs);
  if (!opened) {
    LOG_ERR("MUS", "Cannot open %s", tracks_[index_].c_str());
    status_ = Status::ErrorDecode;
    return false;
  }

  sampleRate_ = source_->sampleRate();
  channels_ = source_->channels();
  bitDepth_ = source_->bitDepth();
  format_ = source_->format();
  if (frameOffset == 0) durationMs_ = source_->durationMs();

  if (channels_ == 0 || !isRateSupported(sampleRate_)) {
    LOG_ERR("MUS", "Unsupported stream: %u Hz / %u ch", static_cast<unsigned>(sampleRate_),
            static_cast<unsigned>(channels_));
    status_ = Status::ErrorRate;
    source_->close();
    return false;
  }

  pcmByteOffset_ = 0;
  ended_ = false;
  // The audio task starts pulling as soon as playMusicPcm() returns, so the
  // session must already be in Playing when the source is handed over.
  status_ = Status::Playing;

  // The only place that couples decoder output to the audio layer. AudioManager
  // programmes the ES8311 clock for whatever rate the file reports.
  if (!HalAudioOutput::playMusicPcm(&MusicSession::readThunk, nullptr, sampleRate_, channels_, volume_)) {
    LOG_ERR("MUS", "Audio output rejected %u Hz / %u ch", static_cast<unsigned>(sampleRate_),
            static_cast<unsigned>(channels_));
    status_ = Status::ErrorDecode;
    ended_ = true;
    source_->close();
    return false;
  }
  return true;
}

bool MusicSession::playIndex(const int index) {
  // Stop the audio task before touching the decoder it is reading from.
  status_ = Status::Stopped;
  ended_ = true;
  stopOutput();
  if (index < 0 || index >= static_cast<int>(tracks_.size())) return false;
  index_ = index;
  resumeOffset_ = 0;
  resumeMs_ = 0;
  source_.reset();
  return startPlayback(0, 0);
}

bool MusicSession::togglePause() {
  if (status_ == Status::Playing) {
    // Freeze the source first so the audio task cannot advance it while the
    // resume position is captured.
    status_ = Status::Paused;
    stopOutput();
    resumeOffset_ = source_ ? source_->nextFrameOffset() : 0;
    resumeMs_ = source_ ? source_->elapsedMs() : 0;
    return true;
  }
  if (status_ == Status::Paused && source_) {
    return startPlayback(resumeOffset_, resumeMs_);
  }
  return false;
}

bool MusicSession::next() {
  if (tracks_.empty()) return false;
  const int nextIndex = (index_ + 1) % static_cast<int>(tracks_.size());
  return playIndex(nextIndex);
}

bool MusicSession::previous() {
  if (tracks_.empty()) return false;
  const int prevIndex = (index_ - 1 + static_cast<int>(tracks_.size())) % static_cast<int>(tracks_.size());
  return playIndex(prevIndex);
}

void MusicSession::stop() {
  status_ = Status::Stopped;
  ended_ = true;
  stopOutput();
  if (source_) source_->close();
  status_ = tracks_.empty() ? Status::Empty : Status::Stopped;
  resumeOffset_ = 0;
  resumeMs_ = 0;
}

void MusicSession::poll() {
  // Decode errors are raised from the audio task's read callback. Once its DMA
  // has drained, release the codec so button cues and the idle audio rail come
  // back instead of staying reserved for a track that will never play.
  if (hasError()) {
    if (!HalAudioOutput::isPlaying()) stopOutput();
    return;
  }
  if (status_ != Status::Playing || advancing_) return;
  if (HalAudioOutput::isPlaying()) return;

  // The audio task drained and exited: the track ended. Advance once; if the
  // next track is unplayable, startPlayback() puts the session in an error
  // state so this poll cannot spin.
  advancing_ = true;
  if (!next()) {
    stopOutput();
    if (!hasError()) status_ = Status::Stopped;
  }
  advancing_ = false;
}

void MusicSession::shutdown() {
  stop();
  tracks_.clear();
  index_ = -1;
  status_ = Status::Empty;
  durationMs_ = 0;
  sampleRate_ = 0;
  channels_ = 0;
  bitDepth_ = 16;
  source_.reset();
}

void MusicSession::stopOutput() { HalAudioOutput::shutdown(); }

uint32_t MusicSession::positionMs() const { return source_ ? source_->elapsedMs() : 0; }

void MusicSession::setVolume(const uint8_t percent) {
  volume_ = percent > 100 ? 100 : percent;
  if (HalAudioOutput::isInitialized()) HalAudioOutput::setVolume(volume_);
}

#endif  // CROSSPOINT_CAP_MUSIC_PLAYER
