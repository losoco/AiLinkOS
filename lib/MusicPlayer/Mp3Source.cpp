#include "Mp3Source.h"

#if CROSSPOINT_CAP_MUSIC_PLAYER

#include <algorithm>
#include <cstring>

namespace {

constexpr uint16_t kBitrateMpeg1L3[16] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0};
constexpr uint16_t kBitrateMpeg2L3[16] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0};
constexpr uint32_t kRateMpeg1[3] = {44100, 48000, 32000};
constexpr uint32_t kRateMpeg2[3] = {22050, 24000, 16000};
constexpr uint32_t kRateMpeg25[3] = {11025, 12000, 8000};

constexpr size_t kId3HeaderBytes = 10;
constexpr size_t kSyncScanLimit = 32768;

uint32_t readBe32(const uint8_t* p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

}  // namespace

Mp3Source::Mp3Source() = default;
Mp3Source::~Mp3Source() { close(); }

bool Mp3Source::readAt(const size_t pos, void* dst, const size_t len) {
  if (!file_.seek(pos)) return false;
  return file_.read(dst, len) == static_cast<int>(len);
}

bool Mp3Source::parseHeader(const uint8_t* h, FrameHeader& out) const {
  if (h[0] != 0xFF || (h[1] & 0xE0) != 0xE0) return false;

  const uint8_t versionBits = (h[1] >> 3) & 0x03;  // 3=MPEG1, 2=MPEG2, 0=MPEG2.5
  const uint8_t layerBits = (h[1] >> 1) & 0x03;
  if (versionBits == 1 || layerBits != 1) return false;  // Layer III only

  const uint8_t bitrateIndex = h[2] >> 4;
  const uint8_t rateIndex = (h[2] >> 2) & 0x03;
  const uint8_t padding = (h[2] >> 1) & 0x01;
  if (bitrateIndex == 0 || bitrateIndex == 15 || rateIndex == 3) return false;

  const bool mpeg1 = versionBits == 3;
  const uint16_t bitrate = mpeg1 ? kBitrateMpeg1L3[bitrateIndex] : kBitrateMpeg2L3[bitrateIndex];
  if (bitrate == 0) return false;

  uint32_t rate = 0;
  if (versionBits == 3) {
    rate = kRateMpeg1[rateIndex];
  } else if (versionBits == 2) {
    rate = kRateMpeg2[rateIndex];
  } else {
    rate = kRateMpeg25[rateIndex];
  }
  if (rate < 8000 || rate > 48000) return false;

  const uint32_t samples = mpeg1 ? 1152 : 576;
  const uint16_t frameBytes =
      static_cast<uint16_t>(((mpeg1 ? 144u : 72u) * bitrate * 1000u / rate) + padding);
  if (frameBytes < 4 || frameBytes > sizeof(frameBuf_)) return false;

  out.sampleRate = rate;
  out.frameBytes = frameBytes;
  out.samples = static_cast<uint16_t>(samples);
  out.channels = ((h[3] >> 6) & 0x03) == 3 ? 1 : 2;
  out.bitrateKbps = static_cast<uint8_t>(bitrate);
  out.mpeg1 = mpeg1;
  return true;
}

bool Mp3Source::findNextFrame(const size_t from, size_t& found, FrameHeader& out) {
  uint8_t window[512];
  size_t pos = from;
  while (pos + 4 <= fileSize_ && pos - from < kSyncScanLimit) {
    const size_t avail = std::min(fileSize_ - pos, sizeof(window));
    if (!readAt(pos, window, avail)) return false;
    for (size_t i = 0; i + 4 <= avail; ++i) {
      if (parseHeader(window + i, out)) {
        found = pos + i;
        return true;
      }
    }
    if (avail <= 4) break;
    pos += avail - 3;  // keep a 3-byte overlap so headers never straddle a refill
  }
  return false;
}

bool Mp3Source::readFrame(const FrameHeader& h) {
  if (!readAt(framePos_, frameBuf_, h.frameBytes)) {
    error_ = true;
    return false;
  }
  pcmSamples_ = 0;
  mp3dec_frame_info_t info{};
  const int samples =
      mp3dec_decode_frame(&decoder_, frameBuf_, h.frameBytes, pcm_, &info);
  framePos_ += h.frameBytes;
  if (samples <= 0) return true;  // bit-reservoir warm-up or a damaged frame

  pcmSamples_ = samples;
  if (info.channels > 0) channels_ = static_cast<uint8_t>(info.channels);
  if (info.hz > 0) sampleRate_ = static_cast<uint32_t>(info.hz);
  elapsedMs_ += static_cast<uint32_t>((static_cast<uint64_t>(samples) * 1000) / sampleRate_);
  return true;
}

void Mp3Source::estimateDuration(const size_t audioStart, const FrameHeader& first) {
  // Xing/Info header carries an exact frame count; otherwise assume CBR from the
  // first frame's bitrate. Either way this is only used for the progress readout.
  const size_t sideInfo = first.mpeg1 ? (first.channels == 1 ? 17 : 32) : (first.channels == 1 ? 9 : 17);
  uint8_t xing[8];
  if (first.frameBytes >= 4 + sideInfo + 8 && readAt(audioStart + 4 + sideInfo, xing, sizeof(xing)) &&
      (memcmp(xing, "Xing", 4) == 0 || memcmp(xing, "Info", 4) == 0)) {
    const uint32_t flags = readBe32(xing + 4);
    uint8_t frames[4];
    if ((flags & 0x01) && readAt(audioStart + 4 + sideInfo + 8, frames, 4)) {
      const uint64_t total = static_cast<uint64_t>(readBe32(frames)) * first.samples * 1000ull /
                             first.sampleRate;
      durationMs_ = static_cast<uint32_t>(total);
      return;
    }
  }

  size_t audioBytes = fileSize_ > audioStart ? fileSize_ - audioStart : 0;
  uint8_t tag[3];
  if (fileSize_ >= 128 && readAt(fileSize_ - 128, tag, sizeof(tag)) && memcmp(tag, "TAG", 3) == 0) {
    audioBytes = audioBytes > 128 ? audioBytes - 128 : 0;
  }
  if (first.bitrateKbps > 0) {
    durationMs_ = static_cast<uint32_t>((static_cast<uint64_t>(audioBytes) * 8) / first.bitrateKbps);
  }
}

bool Mp3Source::open(const std::string& path) {
  close();
  file_ = Storage.open(path.c_str());
  if (!file_.isOpen() || file_.isDirectory()) {
    close();
    return false;
  }
  fileSize_ = file_.size();

  size_t start = 0;
  uint8_t id3[kId3HeaderBytes];
  if (fileSize_ >= kId3HeaderBytes && readAt(0, id3, sizeof(id3)) && memcmp(id3, "ID3", 3) == 0) {
    const size_t size = ((static_cast<size_t>(id3[6] & 0x7F)) << 21) |
                        ((static_cast<size_t>(id3[7] & 0x7F)) << 14) |
                        ((static_cast<size_t>(id3[8] & 0x7F)) << 7) |
                        (static_cast<size_t>(id3[9] & 0x7F));
    start = kId3HeaderBytes + size + ((id3[5] & 0x10) ? 10 : 0);
  }

  FrameHeader first;
  size_t found = 0;
  if (!findNextFrame(start, found, first)) {
    close();
    return false;
  }

  framePos_ = found;
  sampleRate_ = first.sampleRate;
  channels_ = first.channels;
  elapsedMs_ = 0;
  ended_ = false;
  error_ = false;
  pcmSamples_ = 0;
  mp3dec_init(&decoder_);
  estimateDuration(found, first);
  return true;
}

bool Mp3Source::openAt(const std::string& path, const size_t frameOffset, const uint32_t elapsedMs) {
  close();
  file_ = Storage.open(path.c_str());
  if (!file_.isOpen() || file_.isDirectory()) {
    close();
    return false;
  }
  fileSize_ = file_.size();
  if (frameOffset >= fileSize_) {
    close();
    return false;
  }

  FrameHeader header;
  size_t found = 0;
  if (!findNextFrame(frameOffset, found, header) || found != frameOffset) {
    close();
    return false;
  }
  framePos_ = frameOffset;
  sampleRate_ = header.sampleRate;
  channels_ = header.channels;
  elapsedMs_ = elapsedMs;
  ended_ = false;
  error_ = false;
  pcmSamples_ = 0;
  mp3dec_init(&decoder_);
  return true;
}

bool Mp3Source::decodeFrame() {
  if (!file_.isOpen() || ended_) return false;
  for (int attempt = 0; attempt < 8; ++attempt) {
    FrameHeader header;
    size_t found = 0;
    if (!findNextFrame(framePos_, found, header)) {
      // Trailers (ID3v1 is 128 bytes) are a normal end of file; anything with
      // a full frame's worth of bytes left is treated as a corrupt stream.
      const size_t remaining = fileSize_ > framePos_ ? fileSize_ - framePos_ : 0;
      error_ = remaining > 128;
      ended_ = true;
      return false;
    }
    framePos_ = found;
    if (!readFrame(header)) {
      ended_ = true;
      return false;
    }
    if (pcmSamples_ > 0) return true;
  }
  error_ = true;  // eight consecutive frames produced no audio
  ended_ = true;
  return false;
}

void Mp3Source::close() {
  if (file_.isOpen()) file_.close();
  pcmSamples_ = 0;
  framePos_ = 0;
  fileSize_ = 0;
  sampleRate_ = 0;
  durationMs_ = 0;
  elapsedMs_ = 0;
  channels_ = 0;
  ended_ = false;
  error_ = false;
}

#endif  // CROSSPOINT_CAP_MUSIC_PLAYER
