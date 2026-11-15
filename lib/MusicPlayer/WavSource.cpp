#include "WavSource.h"

#if CROSSPOINT_CAP_MUSIC_PLAYER

#include <algorithm>
#include <cstring>

#include "Logging.h"

namespace {

constexpr size_t kRiffHeaderBytes = 12;
constexpr size_t kChunkHeaderBytes = 8;
constexpr size_t kFmtMinBytes = 16;
constexpr size_t kFmtExtBytes = 40;  // WAVE_FORMAT_EXTENSIBLE fmt chunk

// WAVE_FORMAT_EXTENSIBLE sub-format GUID for PCM (Data1..Data4, little endian).
constexpr uint8_t kPcmSubFormat[16] = {0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
                                       0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};

uint32_t readLe32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint16_t readLe16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0]) | static_cast<uint16_t>(static_cast<uint16_t>(p[1]) << 8);
}

}  // namespace

WavSource::~WavSource() { close(); }

bool WavSource::readAt(const size_t pos, void* dst, const size_t len) {
  if (!file_.seek(pos)) return false;
  return file_.read(dst, len) == static_cast<int>(len);
}

// Walks the RIFF chunk list. Two passes over the same file (open + openAt)
// both land here, so the position is recomputed from the header each time.
bool WavSource::parseHeader() {
  dataStart_ = 0;
  dataBytes_ = 0;
  dataPos_ = 0;
  sampleRate_ = 0;
  byteRate_ = 0;
  durationMs_ = 0;
  channels_ = 0;
  blockAlign_ = 0;

  uint8_t riff[kRiffHeaderBytes];
  if (!readAt(0, riff, sizeof(riff)) || memcmp(riff, "RIFF", 4) != 0 ||
      memcmp(riff + 8, "WAVE", 4) != 0) {
    LOG_ERR("MUS", "Not a RIFF/WAVE file");
    return false;
  }

  bool haveFmt = false;
  bool haveData = false;
  uint16_t formatTag = 0;
  uint16_t bitsPerSample = 0;

  size_t pos = kRiffHeaderBytes;
  while (pos + kChunkHeaderBytes <= fileSize_) {
    uint8_t header[kChunkHeaderBytes];
    if (!readAt(pos, header, sizeof(header))) return false;
    const uint32_t size = readLe32(header + 4);
    const size_t payload = pos + kChunkHeaderBytes;

    if (memcmp(header, "fmt ", 4) == 0) {
      uint8_t fmt[kFmtExtBytes] = {};
      const size_t want = std::min<size_t>(size, sizeof(fmt));
      if (want < kFmtMinBytes || !readAt(payload, fmt, want)) {
        LOG_ERR("MUS", "Truncated WAV fmt chunk");
        return false;
      }
      formatTag = readLe16(fmt);
      channels_ = static_cast<uint8_t>(readLe16(fmt + 2));
      sampleRate_ = readLe32(fmt + 4);
      byteRate_ = readLe32(fmt + 8);
      blockAlign_ = readLe16(fmt + 12);
      bitsPerSample = readLe16(fmt + 14);
      if (formatTag == 0xFFFE) {
        // Extensible: the real format lives in the sub-format GUID.
        if (want < kFmtExtBytes || memcmp(fmt + 24, kPcmSubFormat, sizeof(kPcmSubFormat)) != 0) {
          LOG_ERR("MUS", "Unsupported WAVE_FORMAT_EXTENSIBLE sub-format");
          return false;
        }
        formatTag = 1;
      }
      haveFmt = true;
    } else if (memcmp(header, "data", 4) == 0) {
      const size_t remaining = fileSize_ > payload ? fileSize_ - payload : 0;
      // 0 / 0xFFFFFFFF mean "until EOF" for streaming writers.
      if (size == 0 || size == 0xFFFFFFFFu || size > remaining) {
        dataBytes_ = static_cast<uint32_t>(remaining);
      } else {
        dataBytes_ = size;
      }
      dataStart_ = static_cast<uint32_t>(payload);
      haveData = true;
    }

    if (haveFmt && haveData) break;
    pos = payload + size + (size & 1u);  // chunks are padded to an even size
  }

  if (!haveFmt || !haveData || dataBytes_ == 0) {
    LOG_ERR("MUS", "WAV missing fmt/data chunk");
    return false;
  }
  if (formatTag != 1) {
    LOG_ERR("MUS", "Unsupported WAV format tag %u (need PCM)", static_cast<unsigned>(formatTag));
    return false;
  }
  if (bitsPerSample != 16) {
    LOG_ERR("MUS", "Unsupported WAV depth %u (need 16-bit)", static_cast<unsigned>(bitsPerSample));
    return false;
  }
  if (channels_ == 0 || channels_ > 2 || sampleRate_ == 0) {
    LOG_ERR("MUS", "Unsupported WAV layout: %u ch / %u Hz", static_cast<unsigned>(channels_),
            static_cast<unsigned>(sampleRate_));
    return false;
  }
  if (blockAlign_ != channels_ * 2) {
    LOG_ERR("MUS", "WAV blockAlign %u does not match %u ch / 16-bit",
            static_cast<unsigned>(blockAlign_), static_cast<unsigned>(channels_));
    return false;
  }
  if (byteRate_ == 0) byteRate_ = sampleRate_ * blockAlign_;
  durationMs_ = static_cast<uint32_t>(static_cast<uint64_t>(dataBytes_) * 1000 / byteRate_);
  return true;
}

bool WavSource::reopen(const std::string& path) {
  close();
  file_ = Storage.open(path.c_str());
  if (!file_.isOpen() || file_.isDirectory()) {
    close();
    return false;
  }
  fileSize_ = static_cast<uint32_t>(file_.size());
  return parseHeader();
}

bool WavSource::open(const std::string& path) {
  if (!reopen(path)) return false;
  dataPos_ = dataStart_;
  samples_ = 0;
  elapsedMs_ = 0;
  ended_ = false;
  error_ = false;
  return true;
}

bool WavSource::openAt(const std::string& path, const size_t offset, const uint32_t elapsedMs) {
  if (!reopen(path)) return false;
  const uint32_t dataEnd = dataStart_ + dataBytes_;
  if (offset < dataStart_ || offset >= dataEnd) {
    close();
    return false;
  }
  uint32_t start = static_cast<uint32_t>(offset);
  const uint32_t misaligned = (start - dataStart_) % blockAlign_;
  if (misaligned != 0) start -= misaligned;

  dataPos_ = start;
  samples_ = 0;
  elapsedMs_ = elapsedMs;
  ended_ = false;
  error_ = false;
  return true;
}

bool WavSource::decodeFrame() {
  samples_ = 0;
  if (!file_.isOpen() || ended_) return false;

  const uint32_t dataEnd = dataStart_ + dataBytes_;
  if (dataPos_ >= dataEnd) {
    ended_ = true;
    return false;
  }

  size_t want = std::min<size_t>(dataEnd - dataPos_, kChunkBytes);
  want -= want % blockAlign_;  // whole frames only
  if (want == 0) {
    ended_ = true;
    return false;
  }
  if (!file_.seek(dataPos_)) {
    error_ = true;
    ended_ = true;
    return false;
  }
  const int read = file_.read(pcm_, want);
  if (read <= 0) {
    error_ = true;
    ended_ = true;
    return false;
  }

  samples_ = read / blockAlign_;
  dataPos_ += static_cast<uint32_t>(read);
  if (static_cast<size_t>(read) < want) {
    // The declared data chunk was cut short: stop here and let the player show
    // its error page instead of ending as if the file finished cleanly.
    error_ = true;
    ended_ = true;
  }
  elapsedMs_ += static_cast<uint32_t>(static_cast<uint64_t>(read) * 1000 / byteRate_);
  return samples_ > 0;
}

void WavSource::close() {
  if (file_.isOpen()) file_.close();
  fileSize_ = 0;
  dataStart_ = 0;
  dataBytes_ = 0;
  dataPos_ = 0;
  sampleRate_ = 0;
  byteRate_ = 0;
  durationMs_ = 0;
  elapsedMs_ = 0;
  blockAlign_ = 0;
  channels_ = 0;
  samples_ = 0;
  ended_ = false;
  error_ = false;
}

#endif  // CROSSPOINT_CAP_MUSIC_PLAYER
