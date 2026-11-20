#pragma once

#include <HalStorage.h>

#include <cstddef>
#include <cstdint>

// Minimal 16-bit PCM WAV writer for SD recordings.
//
// A recording has no known length when it starts, so open() writes a canonical
// 44-byte header with zeroed RIFF/data sizes and finish() backfills them. The
// file is kept open for the whole recording: re-opening per chunk would cost a
// FAT directory update every few hundred milliseconds.
class WavWriter {
 public:
  bool open(const char* path, uint32_t sampleRate, uint8_t channels);
  bool write(const uint8_t* data, size_t len);
  // Backfills the RIFF sizes and closes. Safe to call twice.
  bool finish();

  bool isOpen() const { return file_.isOpen(); }
  size_t dataBytes() const { return dataBytes_; }

 private:
  HalFile file_;
  size_t dataBytes_ = 0;
};
