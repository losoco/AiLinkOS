#pragma once

#if CROSSPOINT_CAP_RADIO

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

class Client;

// MP3-over-HTTP(S) live stream.
//
// One instance owns one socket and one minimp3 decoder. open() performs the
// HTTP handshake, follows up to three redirects, and decodes enough of the body
// to learn the sample rate, so the caller can program the codec before playback
// starts. readPcm() then blocks until it has filled the buffer, which is what
// the audio task expects.
//
// HLS (.m3u8) is not supported: it needs a playlist parser and a TS demuxer.
class RadioStream {
 public:
  RadioStream();
  ~RadioStream();

  bool open(const std::string& url);
  void close();

  // Interleaved signed 16-bit PCM. Returns bytes written, or 0 when the stream
  // died (the caller shows an error and can retry with open()).
  int readPcm(uint8_t* dst, size_t len);

  uint32_t sampleRate() const { return sampleRate_; }
  uint8_t channels() const { return channels_; }
  bool isOpen() const { return client_ != nullptr; }
  const char* errorText() const { return error_; }

  // Download and buffer telemetry for the player page. Written by the audio
  // task, read by the UI.
  uint32_t bytesPerSecond() const { return bytesPerSecond_.load(std::memory_order_relaxed); }
  uint32_t bufferedBytes() const { return bufferedBytes_.load(std::memory_order_relaxed); }
  uint32_t receivedBytes() const { return receivedBytes_.load(std::memory_order_relaxed); }
  uint16_t bitrateKbps() const { return bitrateKbps_; }

 private:
  struct Url {
    bool tls = false;
    std::string host;
    uint16_t port = 80;
    std::string path;
  };
  static bool parseUrl(const std::string& url, Url& out);
  bool connectAndHandshake(Url url);
  bool readResponse(Url& url, int depth);
  bool readLine(std::string& out, uint32_t timeoutMs);
  bool readExact(uint8_t* dst, size_t len, uint32_t timeoutMs);
  // Socket bytes -> inBuf_, stripping ICY metadata blocks on the way.
  int fillBuffer(uint32_t timeoutMs);
  bool decodeNextFrame();
  void releaseBuffer();

  std::unique_ptr<Client> client_;
  bool tls_ = false;
  uint16_t metaInterval_ = 0;
  uint32_t bytesUntilMeta_ = 0;

  // 64 KiB in PSRAM (about 8 s of 64 kbps audio) absorbs Wi-Fi jitter; the
  // static 8 KiB buffer is the fallback when PSRAM is unavailable.
  static constexpr size_t kStaticBufferBytes = 8192;
  static constexpr size_t kPsramBufferBytes = 65536;
  uint8_t inBufStatic_[kStaticBufferBytes] = {};
  uint8_t* inBuf_ = inBufStatic_;
  size_t inBufSize_ = kStaticBufferBytes;
  size_t inHead_ = 0;
  size_t inTail_ = 0;

  static constexpr size_t kFrameBytes = 2048;
  uint8_t frameBuf_[kFrameBytes] = {};

  static constexpr int kMaxSamples = 1152;
  int16_t pcm_[kMaxSamples * 2] = {};
  int pcmSamples_ = 0;
  int pcmRead_ = 0;

  void* decoder_ = nullptr;  // mp3dec_t, allocated on open
  uint32_t sampleRate_ = 0;
  uint8_t channels_ = 2;
  const char* error_ = "";

  std::atomic<uint32_t> bytesPerSecond_{0};
  std::atomic<uint32_t> bufferedBytes_{0};
  std::atomic<uint32_t> receivedBytes_{0};
  // Audio task only: one-second sliding window for bytesPerSecond_.
  uint32_t speedWindowStartMs_ = 0;
  uint32_t speedWindowBytes_ = 0;
  uint16_t bitrateKbps_ = 0;
};

#endif  // CROSSPOINT_CAP_RADIO
