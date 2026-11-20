#include "RecordingSession.h"

#include <Arduino.h>
#include <HalAudioOutput.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstdio>
#include <cstring>
#include <ctime>

namespace {

void readLe16(const uint8_t* p, int16_t& out) { memcpy(&out, p, sizeof(out)); }

}  // namespace

bool RecordingSession::begin(const uint16_t sampleRate, const uint32_t maxDurationMs) {
  if (state_ == State::Recording || state_ == State::Saving) return false;
  state_ = State::Idle;
  error_ = Error::None;
  frames_ = 0;
  skipLeft_ = 0;
  rampPos_ = 0;
  zeroReads_ = 0;
  path_[0] = '\0';

  if (!Storage.ready()) {
    fail(Error::NoSd);
    return false;
  }

  // No free-space probe here: SDCardManager::getSpace() runs a full
  // freeClusterCount() scan on a cold cache, which stalls recording start for
  // seconds on a large card. A full card surfaces as a short write instead,
  // which finalizes what was captured and reports WriteFailed.

  if (!Storage.ensureDirectoryExists("/rec")) {
    LOG_ERR("REC", "Cannot create /rec");
    fail(Error::OpenFailed);
    return false;
  }
  if (!buildPath()) {
    LOG_ERR("REC", "Cannot find a free recording name");
    fail(Error::OpenFailed);
    return false;
  }
  if (!wav_.open(path_, sampleRate, 1)) {
    fail(Error::OpenFailed);
    return false;
  }
  if (!HalAudioOutput::beginCapture(sampleRate)) {
    LOG_ERR("REC", "beginCapture failed (hasMic=%d)", HalAudioOutput::hasMicrophone() ? 1 : 0);
    wav_.finish();
    Storage.remove(path_);
    fail(Error::MicFailed);
    return false;
  }

  sampleRate_ = sampleRate;
  limitFrames_ = maxDurationMs ? static_cast<uint32_t>(static_cast<uint64_t>(sampleRate) * maxDurationMs / 1000) : 0;
  skipFrames_ = static_cast<uint32_t>(static_cast<uint64_t>(sampleRate) * kStartSkipMs / 1000);
  rampFrames_ = static_cast<uint32_t>(static_cast<uint64_t>(sampleRate) * kRampInMs / 1000);
  state_ = State::Recording;
  LOG_INF("REC", "Recording to %s at %u Hz", path_, static_cast<unsigned>(sampleRate));
  return true;
}

bool RecordingSession::pump() {
  if (state_ != State::Recording) return false;

  if (limitFrames_ != 0 && frames_ >= limitFrames_) {
    end();
    return true;
  }

  const size_t got = HalAudioOutput::readCapture(capture_, sizeof(capture_), kReadTimeoutMs);
  if (got == 0) {
    if (++zeroReads_ >= kMaxZeroReads) {
      LOG_ERR("REC", "Capture stalled");
      HalAudioOutput::endCapture();
      wav_.finish();
      fail(Error::MicFailed);
      return true;
    }
    return false;
  }
  zeroReads_ = 0;

  const size_t frames = got / 4;  // 16-bit stereo frames
  size_t count = 0;
  for (size_t i = 0; i < frames; ++i) {
    if (skipLeft_ < skipFrames_) {
      ++skipLeft_;
      continue;
    }
    if (limitFrames_ != 0 && frames_ + count >= limitFrames_) break;

    // memcpy: the uint8_t buffer is not guaranteed to be 2-byte aligned, and
    // RISC-V faults on a raw cast to int16_t*.
    int16_t left = 0;
    int16_t right = 0;
    readLe16(capture_ + i * 4, left);
    readLe16(capture_ + i * 4 + 2, right);
    int32_t sample = (static_cast<int32_t>(left) + right) / 2;

    if (rampPos_ < rampFrames_) {
      sample = sample * static_cast<int32_t>(rampPos_) / static_cast<int32_t>(rampFrames_);
      ++rampPos_;
    }
    mono_[count++] = static_cast<int16_t>(sample);
  }

  if (count == 0) return false;
  if (!wav_.write(reinterpret_cast<const uint8_t*>(mono_), count * sizeof(int16_t))) {
    HalAudioOutput::endCapture();
    wav_.finish();
    fail(Error::WriteFailed);
    return true;
  }
  frames_ += static_cast<uint32_t>(count);

  if (limitFrames_ != 0 && frames_ >= limitFrames_) {
    end();
    return true;
  }
  return false;
}

bool RecordingSession::end() {
  if (state_ != State::Recording) return state_ == State::Idle;
  state_ = State::Saving;
  HalAudioOutput::endCapture();
  const bool ok = wav_.finish();
  if (!ok) {
    error_ = Error::WriteFailed;
    state_ = State::Error;
    return false;
  }
  // A tap-stop with no captured frames leaves a 44-byte stub that would show up
  // as a zero-length memo; drop it instead of keeping dead entries.
  if (wav_.dataBytes() == 0 && path_[0] != '\0') {
    Storage.remove(path_);
    path_[0] = '\0';
  }
  state_ = State::Idle;
  return true;
}

uint32_t RecordingSession::elapsedMs() const {
  if (sampleRate_ == 0) return 0;
  return static_cast<uint32_t>(static_cast<uint64_t>(frames_) * 1000 / sampleRate_);
}

void RecordingSession::fail(const Error error) {
  error_ = error;
  state_ = State::Error;
}

bool RecordingSession::buildPath() {
  const time_t now = time(nullptr);
  struct tm local{};
  bool haveClock = false;
  if (now > 1700000000 && localtime_r(&now, &local) != nullptr) {
    haveClock = local.tm_year + 1900 >= 2024;
  }

  char base[40];
  if (haveClock) {
    snprintf(base, sizeof(base), "REC_%04d%02d%02d_%02d%02d%02d", local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
             local.tm_hour, local.tm_min, local.tm_sec);
  } else {
    // No valid RTC: the uptime keeps names distinct within a boot, and the
    // exists() loop below covers collisions across boots.
    snprintf(base, sizeof(base), "REC_%lu", static_cast<unsigned long>(millis()));
  }

  for (int i = 0; i < 100; ++i) {
    if (i == 0) {
      snprintf(path_, sizeof(path_), "/rec/%s.wav", base);
    } else {
      snprintf(path_, sizeof(path_), "/rec/%s_%d.wav", base, i);
    }
    if (!Storage.exists(path_)) return true;
  }
  path_[0] = '\0';
  return false;
}
