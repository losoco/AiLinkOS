#pragma once

#if CROSSPOINT_CAP_RADIO

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
  int fillBuffer();
  bool decodeNextFrame();

  std::unique_ptr<Client> client_;
  bool tls_ = false;
  uint16_t metaInterval_ = 0;
  uint32_t bytesUntilMeta_ = 0;

  static constexpr size_t kInBufferBytes = 8192;
  uint8_t inBuf_[kInBufferBytes] = {};
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
};

#endif  // CROSSPOINT_CAP_RADIO
