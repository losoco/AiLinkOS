#include "WavWriter.h"

#include <Logging.h>

#include <cstring>

namespace {

constexpr size_t kWavHeaderBytes = 44;

void putLe16(uint8_t* p, const uint16_t v) {
  p[0] = static_cast<uint8_t>(v & 0xFF);
  p[1] = static_cast<uint8_t>(v >> 8);
}

void putLe32(uint8_t* p, const uint32_t v) {
  p[0] = static_cast<uint8_t>(v & 0xFF);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
  p[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
  p[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

}  // namespace

bool WavWriter::open(const char* path, const uint32_t sampleRate, const uint8_t channels) {
  if (sampleRate == 0 || (channels != 1 && channels != 2)) return false;
  if (!Storage.openFileForWrite("REC", path, file_)) {
    LOG_ERR("REC", "Cannot open %s for writing", path);
    return false;
  }
  dataBytes_ = 0;

  const uint32_t byteRate = sampleRate * channels * 2;
  uint8_t header[kWavHeaderBytes] = {};
  memcpy(header, "RIFF", 4);
  putLe32(header + 4, 36);  // patched by finish()
  memcpy(header + 8, "WAVEfmt ", 8);
  putLe32(header + 16, 16);  // PCM fmt chunk size
  putLe16(header + 20, 1);   // PCM
  putLe16(header + 22, channels);
  putLe32(header + 24, sampleRate);
  putLe32(header + 28, byteRate);
  putLe16(header + 32, static_cast<uint16_t>(channels * 2));
  putLe16(header + 34, 16);
  memcpy(header + 36, "data", 4);
  putLe32(header + 40, 0);  // patched by finish()

  if (file_.write(header, kWavHeaderBytes) != kWavHeaderBytes) {
    LOG_ERR("REC", "Cannot write WAV header to %s", path);
    file_.close();
    return false;
  }
  return true;
}

bool WavWriter::write(const uint8_t* data, const size_t len) {
  if (!file_.isOpen() || data == nullptr || len == 0) return false;
  if (file_.write(data, len) != len) {
    LOG_ERR("REC", "Short write at %u bytes", static_cast<unsigned>(dataBytes_));
    return false;
  }
  dataBytes_ += len;
  return true;
}

bool WavWriter::finish() {
  if (!file_.isOpen()) return true;

  uint8_t size[4];
  putLe32(size, static_cast<uint32_t>(dataBytes_ + 36));
  bool ok = file_.seek(4) && file_.write(size, 4) == 4;
  putLe32(size, static_cast<uint32_t>(dataBytes_));
  ok = file_.seek(40) && file_.write(size, 4) == 4 && ok;
  file_.flush();
  // A member file is closed explicitly: callers reopen or play the path right
  // after finishing, which needs the directory entry updated.
  file_.close();
  return ok;
}
