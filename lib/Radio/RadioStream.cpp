#include "RadioStream.h"

#if CROSSPOINT_CAP_RADIO

#include <Arduino.h>
#include <Logging.h>
#include <Memory.h>
#include <NetworkClientSecure.h>
#include <WiFiClient.h>

#include <algorithm>
#include <cstring>

#include "minimp3.h"

namespace {

// Layer III frame tables. Kept local so this library does not depend on the
// music player's file-based Mp3Source.
constexpr uint16_t kBitrateMpeg1L3[16] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0};
constexpr uint16_t kBitrateMpeg2L3[16] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0};
constexpr uint32_t kRateMpeg1[3] = {44100, 48000, 32000};
constexpr uint32_t kRateMpeg2[3] = {22050, 24000, 16000};
constexpr uint32_t kRateMpeg25[3] = {11025, 12000, 8000};

constexpr uint32_t kConnectTimeoutMs = 8000;
constexpr uint32_t kReadTimeoutMs = 6000;

struct FrameHeader {
  uint32_t sampleRate = 0;
  uint16_t frameBytes = 0;
  uint16_t samples = 0;
  uint8_t channels = 2;
};

bool parseFrameHeader(const uint8_t* h, FrameHeader& out) {
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
  const uint16_t frameBytes = static_cast<uint16_t>(((mpeg1 ? 144u : 72u) * bitrate * 1000u / rate) + padding);
  if (frameBytes < 4) return false;

  out.sampleRate = rate;
  out.frameBytes = frameBytes;
  out.samples = static_cast<uint16_t>(samples);
  out.channels = ((h[3] >> 6) & 0x03) == 3 ? 1 : 2;
  return true;
}

}  // namespace

RadioStream::RadioStream() = default;

RadioStream::~RadioStream() { close(); }

bool RadioStream::parseUrl(const std::string& url, Url& out) {
  const size_t schemeEnd = url.find("://");
  if (schemeEnd == std::string::npos) return false;
  const std::string scheme = url.substr(0, schemeEnd);
  out.tls = scheme == "https";
  if (!out.tls && scheme != "http") return false;

  const size_t hostStart = schemeEnd + 3;
  size_t hostEnd = url.find_first_of("/?#", hostStart);
  if (hostEnd == std::string::npos) hostEnd = url.size();
  std::string authority = url.substr(hostStart, hostEnd - hostStart);
  out.port = out.tls ? 443 : 80;
  const size_t colon = authority.rfind(':');
  if (colon != std::string::npos) {
    out.port = static_cast<uint16_t>(atoi(authority.c_str() + colon + 1));
    authority = authority.substr(0, colon);
  }
  out.host = authority;
  out.path = hostEnd < url.size() ? url.substr(hostEnd) : "/";
  if (out.path.empty() || out.path.front() != '/') out.path.insert(out.path.begin(), '/');
  const size_t cut = out.path.find('#');
  if (cut != std::string::npos) out.path.resize(cut);
  return !out.host.empty() && out.port != 0;
}

void RadioStream::close() {
  if (client_) {
    client_->stop();
    client_.reset();
  }
  if (decoder_) {
    auto* dec = static_cast<mp3dec_t*>(decoder_);
    delete dec;
    decoder_ = nullptr;
  }
  inHead_ = inTail_ = 0;
  pcmSamples_ = pcmRead_ = 0;
  metaInterval_ = 0;
  bytesUntilMeta_ = 0;
  sampleRate_ = 0;
  channels_ = 2;
  error_ = "";
}

bool RadioStream::readLine(std::string& out, const uint32_t timeoutMs) {
  out.clear();
  const uint32_t deadline = millis() + timeoutMs;
  while (millis() < deadline) {
    while (client_ && client_->available() > 0) {
      const int c = client_->read();
      if (c < 0) break;
      if (c == '\n') {
        if (!out.empty() && out.back() == '\r') out.pop_back();
        return true;
      }
      if (out.size() < 512) out.push_back(static_cast<char>(c));
    }
    if (!client_ || !client_->connected()) return false;
    delay(2);
  }
  return false;
}

bool RadioStream::readExact(uint8_t* dst, const size_t len, const uint32_t timeoutMs) {
  size_t got = 0;
  const uint32_t deadline = millis() + timeoutMs;
  while (got < len && millis() < deadline) {
    if (!client_) return false;
    const int n = client_->read(dst + got, len - got);
    if (n > 0) {
      got += static_cast<size_t>(n);
      continue;
    }
    if (!client_->connected()) return false;
    delay(2);
  }
  return got == len;
}

bool RadioStream::readResponse(Url& url, const int depth) {
  std::string line;
  if (!readLine(line, kReadTimeoutMs)) {
    error_ = "no response";
    return false;
  }
  // Some Icecast servers still answer with the pre-HTTP "ICY 200 OK" line.
  const bool icy = line.rfind("ICY", 0) == 0;
  int code = 0;
  if (line.size() > 12) code = atoi(line.c_str() + (icy ? 4 : 9));
  if (!icy && line.rfind("HTTP/", 0) != 0) {
    error_ = "not http";
    return false;
  }

  uint16_t metaInterval = 0;
  std::string location;
  while (true) {
    if (!readLine(line, kReadTimeoutMs)) {
      error_ = "truncated headers";
      return false;
    }
    if (line.empty()) break;
    const size_t colon = line.find(':');
    if (colon == std::string::npos) continue;
    std::string key = line.substr(0, colon);
    std::string value = line.substr(colon + 1);
    while (!value.empty() && value.front() == ' ') value.erase(value.begin());
    for (char& ch : key) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
    if (key == "icy-metaint") {
      metaInterval = static_cast<uint16_t>(atoi(value.c_str()));
    } else if (key == "location") {
      location = value;
    }
  }

  if (code >= 300 && code < 400 && !location.empty() && depth < 3) {
    Url next;
    if (location.rfind("http", 0) == 0) {
      if (!parseUrl(location, next)) return false;
    } else {
      next = url;
      next.path = location.rfind('/', 0) == 0 ? location : "/" + location;
    }
    client_->stop();
    client_.reset();
    return connectAndHandshake(next);
  }
  if (code != 200) {
    error_ = code == 404 ? "not found" : "http error";
    LOG_ERR("RAD", "Stream answered %d", code);
    return false;
  }
  metaInterval_ = metaInterval;
  bytesUntilMeta_ = metaInterval_ ? metaInterval_ : 0;
  return true;
}

bool RadioStream::connectAndHandshake(Url url) {
  const uint32_t freeHeap = ESP.getFreeHeap();
  if (url.tls && freeHeap < 40000) {
    error_ = "low memory";
    return false;
  }

  if (url.tls) {
    auto secure = makeUniqueNoThrow<NetworkClientSecure>();
    if (!secure) {
      error_ = "oom";
      return false;
    }
    // Live radio certificates rotate often and the board has no clock/bundle;
    // the stream is public audio, so pinning is not practical here.
    secure->setInsecure();
    secure->setTimeout(kReadTimeoutMs / 1000);
    client_ = std::move(secure);
  } else {
    auto plain = makeUniqueNoThrow<WiFiClient>();
    if (!plain) {
      error_ = "oom";
      return false;
    }
    plain->setTimeout(kReadTimeoutMs / 1000);
    client_ = std::move(plain);
  }
  tls_ = url.tls;

  if (!client_->connect(url.host.c_str(), url.port)) {
    error_ = "connect failed";
    LOG_ERR("RAD", "Connect %s:%u failed", url.host.c_str(), static_cast<unsigned>(url.port));
    return false;
  }

  std::string request = "GET ";
  request += url.path;
  request += " HTTP/1.0\r\nHost: ";
  request += url.host;
  request += "\r\nUser-Agent: CrossMux-Radio/1.0\r\nAccept: */*\r\nIcy-MetaData: 1\r\nConnection: close\r\n\r\n";
  client_->write(reinterpret_cast<const uint8_t*>(request.c_str()), request.size());
  return readResponse(url, 0);
}

int RadioStream::fillBuffer() {
  // Compact first so there is always room to append; the read loop below never
  // has to reason about a wrapped buffer.
  if (inHead_ > 0) {
    if (inHead_ == inTail_) {
      inHead_ = inTail_ = 0;
    } else {
      memmove(inBuf_, inBuf_ + inHead_, inTail_ - inHead_);
      inTail_ -= inHead_;
      inHead_ = 0;
    }
  }
  if (!client_) return -1;
  if (inTail_ >= sizeof(inBuf_)) return -1;  // a single frame bigger than the buffer

  const uint32_t deadline = millis() + kReadTimeoutMs;
  while (millis() < deadline) {
    if (metaInterval_ && bytesUntilMeta_ == 0) {
      uint8_t lenByte = 0;
      if (!readExact(&lenByte, 1, kReadTimeoutMs)) return -1;
      size_t left = static_cast<size_t>(lenByte) * 16;
      uint8_t scratch[64];
      while (left > 0) {
        const size_t chunk = std::min(left, sizeof(scratch));
        if (!readExact(scratch, chunk, kReadTimeoutMs)) return -1;
        left -= chunk;
      }
      bytesUntilMeta_ = metaInterval_;
    }

    size_t want = sizeof(inBuf_) - inTail_;
    if (metaInterval_) want = std::min<size_t>(want, bytesUntilMeta_);
    const int n = client_->read(inBuf_ + inTail_, want);
    if (n > 0) {
      inTail_ += static_cast<size_t>(n);
      if (metaInterval_) bytesUntilMeta_ -= static_cast<uint32_t>(n);
      return n;
    }
    if (!client_->connected()) return -1;
    delay(2);
  }
  return -1;
}

bool RadioStream::decodeNextFrame() {
  FrameHeader header;
  // Find a sync word; garbage between frames (and the tail of a torn read) is
  // skipped instead of being fed to the decoder.
  while (true) {
    if (inTail_ - inHead_ < 4) {
      if (fillBuffer() <= 0) return false;
      continue;
    }
    if (parseFrameHeader(inBuf_ + inHead_, header)) break;
    ++inHead_;
  }

  while (inTail_ - inHead_ < header.frameBytes) {
    if (inHead_ > 0) {
      memmove(inBuf_, inBuf_ + inHead_, inTail_ - inHead_);
      inTail_ -= inHead_;
      inHead_ = 0;
    }
    const int n = fillBuffer();
    if (n <= 0) return false;
  }

  if (header.frameBytes > kFrameBytes) {
    inHead_ += header.frameBytes;
    return true;  // can't happen for Layer III, but never overflow the buffer
  }
  memcpy(frameBuf_, inBuf_ + inHead_, header.frameBytes);
  inHead_ += header.frameBytes;

  mp3dec_frame_info_t info{};
  const int samples = mp3dec_decode_frame(static_cast<mp3dec_t*>(decoder_), frameBuf_, header.frameBytes, pcm_, &info);
  if (samples <= 0) return true;  // bit-reservoir warm-up or a damaged frame

  pcmSamples_ = samples;
  pcmRead_ = 0;
  if (info.channels > 0) channels_ = static_cast<uint8_t>(info.channels);
  if (info.hz > 0 && sampleRate_ == 0) {
    sampleRate_ = static_cast<uint32_t>(info.hz);
    LOG_INF("RAD", "Stream %u Hz %u ch", static_cast<unsigned>(sampleRate_), static_cast<unsigned>(channels_));
  }
  return true;
}

bool RadioStream::open(const std::string& url) {
  close();
  Url parsed;
  if (!parseUrl(url, parsed)) {
    error_ = "bad url";
    return false;
  }
  auto* decoder = new (std::nothrow) mp3dec_t();
  if (!decoder) {
    error_ = "oom";
    return false;
  }
  mp3dec_init(decoder);
  decoder_ = decoder;

  if (!connectAndHandshake(parsed)) {
    close();
    return false;
  }

  // Decode ahead until the parameters are known; a stream always has a frame
  // within the first few kilobytes.
  for (int attempt = 0; attempt < 40 && sampleRate_ == 0; ++attempt) {
    if (!decodeNextFrame()) {
      error_ = "no audio";
      close();
      return false;
    }
  }
  if (sampleRate_ == 0) {
    error_ = "no audio";
    close();
    return false;
  }
  return true;
}

int RadioStream::readPcm(uint8_t* dst, const size_t len) {
  size_t written = 0;
  while (written < len) {
    if (pcmRead_ >= pcmSamples_) {
      pcmRead_ = pcmSamples_ = 0;
      if (!decodeNextFrame()) {
        error_ = "stream lost";
        return 0;
      }
      if (pcmSamples_ == 0) continue;
    }
    const size_t available = static_cast<size_t>(pcmSamples_ - pcmRead_) * channels_ * sizeof(int16_t);
    const size_t chunk = std::min(available, len - written);
    memcpy(dst + written, pcm_ + pcmRead_ * channels_, chunk);
    written += chunk;
    pcmRead_ += static_cast<int>(chunk / (channels_ * sizeof(int16_t)));
  }
  return static_cast<int>(written);
}

#endif  // CROSSPOINT_CAP_RADIO
