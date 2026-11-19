#include "MusicPlayerActivity.h"

#if CROSSPOINT_CAP_MUSIC_PLAYER

#include <Arduino.h>
#include <Bitmap.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <MusicSession.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>

#include "CrossPointSettings.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/MusicCoverLoader.h"

namespace {

constexpr uint32_t kVolumeOverlayMs = 3000;
constexpr int kVolumeStep = 5;
// Windowed FAST updates and the 1 Hz progress line leave a slowly accumulating
// residue, bounded by this counter. It only counts windowed FAST frames: a track
// change drives a full-screen HALF clean of its own and resets the counter.
constexpr uint8_t kRefreshCleanupInterval = 180;
// Breathing room between the header rule and the top of the artwork. Applied in
// both layouts and subtracted from the height the cover may use, so the slot,
// text block, progress bar and time row stay inside the content area.
constexpr int kCoverTopGap = 10;

#if CROSSMUX_DEBUG_SERIAL_COMMANDS
// Set from the serial command channel; consumed by the next loop().
bool coverRecheckRequested = false;
bool rotateRequested = false;
#endif

// Settings writes are flash-heavy; only persist when the level really changed
// and the user is done adjusting it.
void persistVolume() {
  if (SETTINGS.musicVolume == MUSIC_SESSION.volume()) return;
  SETTINGS.musicVolume = MUSIC_SESSION.volume();
  SETTINGS.saveToFile();
}

void formatClock(const uint32_t ms, char* out, const size_t outLen) {
  const uint32_t totalSeconds = ms / 1000;
  snprintf(out, outLen, "%u:%02u", static_cast<unsigned>(totalSeconds / 60), static_cast<unsigned>(totalSeconds % 60));
}

const char* statusLabel(const MusicSession::Status status) {
  switch (status) {
    case MusicSession::Status::Playing:
      return tr(STR_MUSIC_PLAYING);
    case MusicSession::Status::Paused:
      return tr(STR_MUSIC_PAUSED);
    case MusicSession::Status::Stopped:
    case MusicSession::Status::Ready:
    case MusicSession::Status::Empty:
      return tr(STR_MUSIC_STOPPED);
    case MusicSession::Status::ErrorDecode:
      return tr(STR_MUSIC_ERR_DECODE);
    case MusicSession::Status::ErrorRate:
      return tr(STR_MUSIC_ERR_RATE);
  }
  return "";
}

void drawCenteredLine(const GfxRenderer& renderer, const Rect& bounds, const int fontId, const int y, const char* text,
                      const EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
  const int width = renderer.getTextWidth(fontId, text, style);
  renderer.drawText(fontId, bounds.x + (bounds.width - width) / 2, y, text, true, style);
}

// The lyric view lifts the track label 5 px above the info view's title anchor,
// which also gives the scrolling window that much more room above the bar. Kept
// as one function so drawing and the dirty rectangle cannot drift apart.
int lyricLabelY(const Rect& textArea) { return std::max(textArea.y, textArea.y + textArea.height * 6 / 100 - 5); }

// Geometry of the lyric view, shared by drawing and the dirty rectangle.
//
// With a stacked cover the title is centred in the gap between the artwork's
// bottom edge (`topAnchor`) and the first lyric row, and the row window is
// anchored to the transport block instead of the title, so the rows fill the
// space down to the bar instead of stacking right under the label.
struct LyricLayout {
  int labelY;
  int top;
  int rows;
};

LyricLayout measureLyricLayout(const GfxRenderer& renderer, const Rect& textArea, const int topAnchor, const int barTop,
                               const bool centerTitle) {
  constexpr int kMaxRows = 7;
  constexpr int kMinGap = 6;
  const int labelH = renderer.getLineHeight(SMALL_FONT_ID);
  const int lineH = renderer.getLineHeight(NOTOSANS_12_FONT_ID);
  const int bottom = barTop - 8;

  if (!centerTitle) {
    const int labelY = lyricLabelY(textArea);
    const int top = labelY + labelH + kMinGap;
    const int rows = std::clamp((bottom - top) / lineH, 1, kMaxRows);
    return {labelY, top, rows};
  }

  const int minTop = textArea.y + labelH + kMinGap;
  int rows = std::clamp((bottom - minTop) / lineH, 1, kMaxRows);
  if (rows % 2 == 0 && rows > 1) --rows;
  const int top = std::max(minTop, bottom - rows * lineH);
  const int labelY = topAnchor + std::max(0, (top - topAnchor - labelH) / 2);
  return {labelY, top, rows};
}

}  // namespace

#if CROSSMUX_DEBUG_SERIAL_COMMANDS
void musicDebugRequestCoverRecheck() { coverRecheckRequested = true; }
void musicDebugRequestRotate() { rotateRequested = true; }
#endif

void MusicPlayerActivity::onEnter() {
  Activity::onEnter();
  // Apps can be reached from a landscape reader, so remember what the panel was
  // doing and put the player in its own remembered orientation.
  previousOrientation_ = renderer.getOrientation();
  renderer.setOrientation(
      static_cast<GfxRenderer::Orientation>(SETTINGS.musicOrientation % CrossPointSettings::ORIENTATION_COUNT));
#if CROSSMUX_DEBUG_SERIAL_COMMANDS
  // A rotation requested while another screen was visible must not fire here.
  rotateRequested = false;
#endif
  lastIndex_ = MUSIC_SESSION.currentIndex();
  lastStatus_ = static_cast<int>(MUSIC_SESSION.status());
  lastShownSecond_ = static_cast<int>(MUSIC_SESSION.positionMs() / 1000);
  volumeOverlay_ = false;
  coverPath_.clear();
  coverPending_ = true;
  lyrics_.clear();
  lyricsPending_ = true;
  lastLyricIndex_ = -2;
  fullDirty_ = true;
  requestUpdate();
}

void MusicPlayerActivity::rotateOrientation() {
  const uint8_t next = (static_cast<uint8_t>(renderer.getOrientation()) + 1) % CrossPointSettings::ORIENTATION_COUNT;
  SETTINGS.musicOrientation = next;
  SETTINGS.saveToFile();
  renderer.setOrientation(static_cast<GfxRenderer::Orientation>(next));
  LOG_INF("MUS", "orientation %u (%ux%u)", static_cast<unsigned>(next),
          static_cast<unsigned>(renderer.getScreenWidth()), static_cast<unsigned>(renderer.getScreenHeight()));
  // The cover slot, text block and hints all move, and the cached artwork tile
  // belongs to the old geometry. Rebuild the page and clean it with one
  // full-screen HALF frame, same as a track change.
  lastShownSecond_ = -1;
  orientationDirty_ = true;
  coverPending_ = true;
  coverTileValid_ = false;
  requestUpdate();
}

void MusicPlayerActivity::loop() {
  const uint32_t now = millis();

  // Auto-advance happens here, not in the audio task: the session only reports
  // that the output drained, and the UI decides when to start the next file.
  MUSIC_SESSION.poll();

  const int index = MUSIC_SESSION.currentIndex();
  const int status = static_cast<int>(MUSIC_SESSION.status());
  if (index != lastIndex_ || status != lastStatus_) {
    const bool trackChanged = index != lastIndex_;
#if CROSSMUX_DEBUG_SERIAL_COMMANDS
    LOG_INF("DBG", "Session change index %d->%d status %d->%d", lastIndex_, index, lastStatus_, status);
#endif
    lastIndex_ = index;
    lastStatus_ = status;
    // A skip is the one moment a full-screen clean is welcome: the artwork,
    // title, status, bar and times all change anyway, so render() rebuilds the
    // page and pushes it with a single HALF waveform. Status-only changes still
    // take a flash-free full FAST frame because only the footer hints move.
    if (trackChanged) {
      lastShownSecond_ = -1;
      trackDirty_ = true;
      coverPending_ = true;
      resetLyricsForTrack();
    } else {
      fullDirty_ = true;
    }
    requestUpdate();
  }

  // Volume feedback hides itself; a new wheel press just refreshes the timer.
  if (volumeOverlay_ && now >= overlayDeadlineMs_) closeVolumeOverlay();

  // Track skip rides the wheel hold. This panel does not deliver Left/Right
  // long presses: after 650 ms the SDK converts a held direction key into an
  // Up/Down press (and the release becomes an Up/Down release), so these edges
  // are exactly "held past the threshold", fire once per hold, and can never
  // also step the volume on release.
  if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
#if CROSSMUX_DEBUG_SERIAL_COMMANDS
    LOG_INF("DBG", "Skip gesture prev at %u ms", static_cast<unsigned>(now));
#endif
    if (MUSIC_SESSION.trackCount() > 0) {
      MUSIC_SESSION.previous();
      // Adopt the new track here so the change detector below does not schedule
      // a second repaint of the same page.
      lastIndex_ = MUSIC_SESSION.currentIndex();
      lastStatus_ = static_cast<int>(MUSIC_SESSION.status());
      lastShownSecond_ = -1;
      trackDirty_ = true;
      coverPending_ = true;
      resetLyricsForTrack();
      requestUpdate();
    }
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
#if CROSSMUX_DEBUG_SERIAL_COMMANDS
    LOG_INF("DBG", "Skip gesture next at %u ms", static_cast<unsigned>(now));
#endif
    if (MUSIC_SESSION.trackCount() > 0) {
      MUSIC_SESSION.next();
      lastIndex_ = MUSIC_SESSION.currentIndex();
      lastStatus_ = static_cast<int>(MUSIC_SESSION.status());
      lastShownSecond_ = -1;
      trackDirty_ = true;
      coverPending_ = true;
      resetLyricsForTrack();
      requestUpdate();
    }
    return;
  }

#if CROSSMUX_DEBUG_SERIAL_COMMANDS
  if (rotateRequested) {
    rotateRequested = false;
    rotateOrientation();
    return;
  }
#endif

  // Long presses first: wasLongPressed() consumes the release, so the same
  // gesture cannot also toggle playback or leave the screen.
  if (mappedInput.wasLongPressed(MappedInputManager::Button::Back, 600)) {
    // Explicit stop: the session keeps its playlist/volume but releases the
    // codec, so the list rows fall back to their format badge.
    MUSIC_SESSION.stop();
    finish();
    return;
  }

  // Holding Confirm rotates the page a quarter turn: 480x800 portrait with the
  // cover above the text, 800x480 landscape with the cover beside it, then the
  // two inverted variants. The choice is remembered for the next visit.
  if (mappedInput.wasLongPressed(MappedInputManager::Button::Confirm, 600)) {
    rotateOrientation();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (MUSIC_SESSION.isActive()) {
      MUSIC_SESSION.togglePause();
      // Only the status word and the footer label move; a FAST full frame is
      // enough and stays flash-free.
      fullDirty_ = true;
      requestUpdate();
    }
    return;
  }
  // Short press on the wheel steps the volume; the overlay shows the level.
  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    changeVolume(-kVolumeStep);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    changeVolume(kVolumeStep);
    return;
  }

  // The progress line only changes once a second; repainting faster would just
  // burn e-ink cycles. Lyrics follow the same cadence: only a row change earns a
  // windowed refresh, so a 3-minute track costs a few dozen fast frames.
  if (MUSIC_SESSION.isPlaying()) {
    const uint32_t position = MUSIC_SESSION.positionMs();
    const int second = static_cast<int>(position / 1000);
    if (second != lastShownSecond_) {
      lastShownSecond_ = second;
      progressDirty_ = true;
      requestUpdate();
    }
    if (!lyrics_.empty()) {
      const int lyricIndex = lyrics_.indexAt(position);
      if (lyricIndex != lastLyricIndex_) {
        lastLyricIndex_ = lyricIndex;
        lyricsDirty_ = true;
        requestUpdate();
      }
    }
  }
}

bool MusicPlayerActivity::preventAutoSleep() { return MUSIC_SESSION.isPlaying(); }

void MusicPlayerActivity::onExit() {
  // Leaving before the overlay times out must still keep the new level.
  persistVolume();
  // Restore the panel before the next screen draws; the list re-applies the
  // music orientation itself, but Apps/Home expect whatever they had.
  renderer.setOrientation(previousOrientation_);
  lyrics_.clear();
  Activity::onExit();
}

void MusicPlayerActivity::prepareForSleep() {
  persistVolume();
  MUSIC_SESSION.shutdown();
}

void MusicPlayerActivity::openVolumeOverlay() {
  volumeOverlay_ = true;
  overlayDeadlineMs_ = millis() + kVolumeOverlayMs;
  volumeDirty_ = true;
  requestUpdate();
}

void MusicPlayerActivity::closeVolumeOverlay() {
  if (!volumeOverlay_) return;
  volumeOverlay_ = false;
  persistVolume();
  volumeDirty_ = true;
  requestUpdate();
}

void MusicPlayerActivity::changeVolume(const int delta) {
  const int next = std::clamp(static_cast<int>(MUSIC_SESSION.volume()) + delta, 0, 100);
#if CROSSMUX_DEBUG_SERIAL_COMMANDS
  LOG_INF("DBG", "Volume gesture delta=%d -> %d at %u ms", delta, next, static_cast<unsigned>(millis()));
#endif
  MUSIC_SESSION.setVolume(static_cast<uint8_t>(next));
  openVolumeOverlay();
}

// The table and text blob are released here, not just flagged for reload, so a
// skip never holds two tracks' lyrics in RAM at once.
void MusicPlayerActivity::resetLyricsForTrack() {
  lyrics_.clear();
  lyricsPending_ = true;
  lastLyricIndex_ = -2;
  lyricsDirty_ = false;
}

void MusicPlayerActivity::loadLyricsIfPending() {
  if (!lyricsPending_) return;
  lyricsPending_ = false;
  lastLyricIndex_ = -2;
  const std::string& track = MUSIC_SESSION.currentPath();
  if (track.empty()) return;
  if (lyrics_.loadForTrack(track)) {
    LOG_INF("LRC", "Lyrics ready: %u lines for %s", static_cast<unsigned>(lyrics_.size()), track.c_str());
  }
}

void MusicPlayerActivity::drawLyricsBlock(GfxRenderer& renderer, const Rect& textArea, const int topAnchor,
                                          const bool centerTitle, const char* title, const int barTop) {
  // The current row stays centred, so the window is odd and never taller than
  // seven rows; on very short text areas it shrinks rather than overlapping the
  // transport block below.
  const LyricLayout layout = measureLyricLayout(renderer, textArea, topAnchor, barTop, centerTitle);
  const std::string label = renderer.truncatedText(SMALL_FONT_ID, title, textArea.width);
  drawCenteredLine(renderer, textArea, SMALL_FONT_ID, layout.labelY, label.c_str());

  const int lineH = renderer.getLineHeight(NOTOSANS_12_FONT_ID);
  const int total = static_cast<int>(lyrics_.size());
  const int current = lyrics_.indexAt(MUSIC_SESSION.positionMs());
  int rows = layout.rows;
  int start = 0;
  if (total > rows) {
    start = std::clamp(current - rows / 2, 0, total - rows);
  } else {
    rows = total;
  }

  for (int i = start; i < start + rows; ++i) {
    const int y = layout.top + (i - start) * lineH;
    const std::string text =
        renderer.truncatedText(NOTOSANS_12_FONT_ID, lyrics_.line(static_cast<size_t>(i)), textArea.width - 16);
    if (i == current) {
      renderer.fillRect(textArea.x, y - 2, textArea.width, lineH + 2, true);
      renderer.drawText(NOTOSANS_12_FONT_ID, textArea.x + 8, y, text.c_str(), false);
    } else {
      renderer.drawText(NOTOSANS_12_FONT_ID, textArea.x + 8, y, text.c_str(), true);
    }
  }
}

void MusicPlayerActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);

  const int contentTop = safe.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int footerReserve = GUI.hintsRowDrawn() ? metrics.buttonHintsHeight : 0;
  const int contentBottom = safe.y + safe.height - footerReserve - metrics.verticalSpacing;
  const Rect content{safe.x + metrics.contentSidePadding, contentTop,
                     std::max(1, safe.width - metrics.contentSidePadding * 2), std::max(1, contentBottom - contentTop)};

  // Cover generation runs first: the converters borrow the framebuffer, and the
  // frame is rebuilt from scratch right below once the loan has ended. The
  // caller already dirtied the right area (full frame on entry, the content
  // rectangle on a skip), so resolving the cover must not widen the refresh.
  const CoverLayout layout = measureCoverLayout(content);
  const int slot = layout.side;
#if CROSSMUX_DEBUG_SERIAL_COMMANDS
  const bool recheckCover = coverRecheckRequested;
  coverRecheckRequested = false;
#else
  const bool recheckCover = false;
#endif
  // A new track (or a forced recheck) is the only reason to go back to the card
  // for artwork. Every other frame replays the snapshot taken last time.
  bool coverSourceChanged = false;
  if (coverPending_ || recheckCover) {
    coverPending_ = false;
    if (recheckCover) fullDirty_ = true;
    coverSourceChanged = true;
    const std::string& track = MUSIC_SESSION.currentPath();
    if (slot > 0 && !track.empty()) {
      GfxRenderer::FrameBufferLoan loan(renderer);
      coverPath_ = MusicCoverLoader::ensureCover(track, slot);
    } else {
      coverPath_.clear();
    }
  }

  // A cached cover can disappear between renders (card swapped, file removed).
  // Probe before committing to the two-column layout so the text re-centres
  // instead of leaving a blank column.
  bool coverMode = slot > 0 && !coverPath_.empty();
  if (coverMode) {
    HalFile probe;
    coverMode = Storage.openFileForRead("MUS", coverPath_, probe);
    if (coverMode) {
      Bitmap bitmap(probe);
      const BmpReaderError header = bitmap.parseHeaders();
      coverMode = header == BmpReaderError::Ok && bitmap.is1Bit();
      if (!coverMode) {
        LOG_ERR("MUS", "Cover probe failed (%s, bpp=%u): %s", coverPath_.c_str(),
                static_cast<unsigned>(bitmap.getBpp()), Bitmap::errorToString(header));
      }
    } else {
      LOG_ERR("MUS", "Cover probe cannot open %s", coverPath_.c_str());
    }
    if (!coverMode) {
      coverPath_.clear();
      fullDirty_ = true;
    }
  }
  LOG_INF("MUS", "Layout content=%dx%d slot=%d stacked=%d cover=%d", content.width, content.height, slot,
          layout.stacked ? 1 : 0, coverMode ? 1 : 0);

  // The cover keeps its square aspect in both layouts: beside the text on wide
  // content, centred above it when there is no room for a second column.
  Rect coverRect{};
  Rect textArea = content;
  if (coverMode) {
    if (layout.stacked) {
      const int top = content.y + kCoverTopGap;
      coverRect = Rect{content.x + (content.width - slot) / 2, top, slot, slot};
      textArea = Rect{content.x, top + slot + 24, content.width, content.height - kCoverTopGap - slot - 24};
    } else {
      const int available = content.height - kCoverTopGap;
      coverRect = Rect{content.x, content.y + kCoverTopGap + (available - slot) / 2, slot, slot};
      textArea = Rect{content.x + slot + 24, content.y, content.width - slot - 24, content.height};
    }
  }

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{safe.x, safe.y + metrics.topPadding, safe.width, metrics.headerHeight},
                 tr(STR_MUSIC_TITLE));

  // The cached BMP is already slot-sized (the converters crop to fill), so this
  // is a 1:1 centre crop, not a second resample. The pixels are snapshotted the
  // first time and replayed for every later frame: a periodic refresh must not
  // reach the card, and must produce byte-identical artwork.
  if (coverMode) {
    const bool samePlace = coverTileValid_ && coverTileRect_.x == coverRect.x && coverTileRect_.y == coverRect.y &&
                           coverTileRect_.width == coverRect.width && coverTileRect_.height == coverRect.height;
    if (samePlace && !coverSourceChanged) {
      if (renderer.copyBufferToRegion(coverRect.x, coverRect.y, coverRect.width, coverRect.height, coverTile_.get(),
                                      coverTileBytes_)) {
        LOG_DBG("MUS", "Cover restore rect=%d,%d,%dx%d bytes=%u", coverRect.x, coverRect.y, coverRect.width,
                coverRect.height, static_cast<unsigned>(coverTileBytes_));
      } else {
        coverTileValid_ = false;  // snapshot unusable; next frame re-reads the BMP
      }
    }
    if (!samePlace || coverSourceChanged) {
      HalFile cover;
      if (Storage.openFileForRead("MUS", coverPath_, cover)) {
        Bitmap bitmap(cover);
        if (bitmap.parseHeaders() == BmpReaderError::Ok) {
          renderer.drawBitmapCropToFill(bitmap, coverRect.x, coverRect.y, coverRect.width, coverRect.height);
          renderer.drawRect(coverRect.x, coverRect.y, coverRect.width, coverRect.height, true);
          LOG_INF("MUS", "Cover draw %ux%u bpp=%u rect=%d,%d,%dx%d src=%s", static_cast<unsigned>(bitmap.getWidth()),
                  static_cast<unsigned>(bitmap.getHeight()), static_cast<unsigned>(bitmap.getBpp()), coverRect.x,
                  coverRect.y, coverRect.width, coverRect.height, coverPath_.c_str());
          captureCoverTile(coverRect);
        } else {
          LOG_ERR("MUS", "Cover header unreadable at draw time: %s", coverPath_.c_str());
        }
      } else {
        LOG_ERR("MUS", "Cover vanished at draw time: %s", coverPath_.c_str());
      }
    }
  } else {
    coverTileValid_ = false;
  }

  // With a cover the text moves into the space the cover leaves; without one it
  // keeps the full width, so no hole is left behind.
  char title[192] = {};
  const int index = MUSIC_SESSION.currentIndex();
  const size_t count = MUSIC_SESSION.trackCount();
  if (index >= 0 && static_cast<size_t>(index) < count) {
    MUSIC_SESSION.trackName(static_cast<size_t>(index), title, sizeof(title));
  }

  // Lyrics load lazily on the first frame after a track change, before any
  // drawing: the .lrc is a text sidecar, but it is still a card read.
  loadLyricsIfPending();
  const bool lyricsView = !lyrics_.empty();

  // Progress + times keep one anchor in both views: the lyric window ends where
  // the bar begins, so toggling the view never moves the transport block.
  const uint32_t position = MUSIC_SESSION.positionMs();
  const uint32_t duration = MUSIC_SESSION.durationMs();
  const int barTop = textArea.y + textArea.height * 64 / 100;

  // With a stacked cover the title floats midway between the artwork's bottom
  // edge and the first lyric row; without one (or in the side-by-side layout)
  // it keeps the compact placement above the rows.
  const bool lyricTitleCentred = coverMode && layout.stacked;
  const int lyricAnchor = lyricTitleCentred ? coverRect.y + coverRect.height : textArea.y;

  if (lyricsView) {
    drawLyricsBlock(renderer, textArea, lyricAnchor, lyricTitleCentred, title, barTop);
  } else {
    // Song title: up to two lines, centred in the upper third of whatever space
    // the cover leaves for text.
    const Rect titleRect{textArea.x, textArea.y + textArea.height * 6 / 100, textArea.width,
                         textArea.height * 34 / 100};
    UITheme::drawCenteredWrappedText(renderer, titleRect, UI_12_FONT_ID, title, 2, true, EpdFontFamily::REGULAR,
                                     UITheme::TextVerticalAlignment::CENTER);

    // Status line: state + container format + stream parameters + track position.
    // Everything comes from the decoder's own report, never from the extension.
    char status[96] = {};
    const uint32_t rate = MUSIC_SESSION.sampleRate();
    if (rate > 0 && count > 0) {
      const unsigned tenthKhz = static_cast<unsigned>(rate / 100);
      const char* container = MUSIC_SESSION.format() == AudioFormat::Wav ? "WAV" : "MP3";
      if (MUSIC_SESSION.format() == AudioFormat::Wav) {
        snprintf(status, sizeof(status), "%s  |  %s %u.%u kHz %u-bit %uch  |  %d / %u",
                 statusLabel(MUSIC_SESSION.status()), container, tenthKhz / 10, tenthKhz % 10,
                 static_cast<unsigned>(MUSIC_SESSION.bitDepth()), static_cast<unsigned>(MUSIC_SESSION.channels()),
                 index + 1, static_cast<unsigned>(count));
      } else {
        snprintf(status, sizeof(status), "%s  |  %s %u.%u kHz  |  %d / %u", statusLabel(MUSIC_SESSION.status()),
                 container, tenthKhz / 10, tenthKhz % 10, index + 1, static_cast<unsigned>(count));
      }
    } else {
      snprintf(status, sizeof(status), "%s", statusLabel(MUSIC_SESSION.status()));
    }
    const int statusY = titleRect.y + titleRect.height + static_cast<int>(renderer.getLineHeight(SMALL_FONT_ID) / 2);
    drawCenteredLine(renderer, textArea, SMALL_FONT_ID, statusY, status);
  }

  // Progress + times.
  const int barHeight = GUI.measureProgressBarHeight(renderer, 10, false);
  if (duration > 0) {
    GUI.drawProgressBar(renderer, Rect{textArea.x, barTop, textArea.width, barHeight},
                        std::min<size_t>(position, duration), duration, false);
  }

  char elapsed[16] = {};
  char total[16] = {};
  formatClock(position, elapsed, sizeof(elapsed));
  formatClock(duration, total, sizeof(total));
  const int timesY = barTop + barHeight + std::max(4, metrics.verticalSpacing);
  const int totalWidth = renderer.getTextWidth(SMALL_FONT_ID, total);
  renderer.drawText(SMALL_FONT_ID, textArea.x, timesY, elapsed, true);
  renderer.drawText(SMALL_FONT_ID, textArea.x + textArea.width - totalWidth, timesY, total, true);

  if (volumeOverlay_) drawVolumeOverlay(content);

  const char* playLabel = MUSIC_SESSION.isPlaying() ? tr(STR_MUSIC_PAUSE) : tr(STR_MUSIC_PLAY);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), playLabel, tr(STR_MUSIC_VOL_DOWN), tr(STR_MUSIC_VOL_UP));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  // The page is always redrawn in full (so the framebuffer and the panel baseline
  // stay aligned) and only the area that changed is pushed. Three kinds of frame:
  //
  //  * a track change, or the periodic clean, drives the whole screen with HALF —
  //    the only waveform here that really erases what was shown before;
  //  * a status/footer change drives a full-frame FAST, which is flash-free;
  //  * a 1 Hz progress tick or a volume step drives just its own window.
  //
  // A track change is the only gesture that earns a HALF: it replaces the artwork,
  // title, status, bar and times at once, so the clean is not an extra frame — it
  // is the frame that was going to be drawn anyway, pushed with the waveform that
  // leaves no ghost of the previous track. The cover is painted into the same
  // framebuffer before this single push, so there is never a white frame in
  // between. Progress ticks and volume steps must never trigger a HALF.
  const int bandTop = barTop - 2;
  const int bandBottom = timesY + renderer.getLineHeight(SMALL_FONT_ID) + 2;

  Rect dirty{};
  bool haveDirty = false;
  const auto addDirty = [&dirty, &haveDirty](const int x, const int y, const int w, const int h) {
    if (w <= 0 || h <= 0) return;
    if (!haveDirty) {
      dirty = Rect{x, y, w, h};
      haveDirty = true;
      return;
    }
    const int x1 = std::min(dirty.x, x);
    const int y1 = std::min(dirty.y, y);
    const int x2 = std::max(dirty.x + dirty.width, x + w);
    const int y2 = std::max(dirty.y + dirty.height, y + h);
    dirty = Rect{x1, y1, x2 - x1, y2 - y1};
  };

  if (progressDirty_) addDirty(textArea.x, bandTop, textArea.width, bandBottom - bandTop);
  // A lyric row change repaints the window above the transport block only; the
  // title label at its top is stable, so the refresh starts below it.
  if (lyricsDirty_ && lyricsView) {
    const LyricLayout lyrics = measureLyricLayout(renderer, textArea, lyricAnchor, barTop, lyricTitleCentred);
    addDirty(textArea.x, lyrics.top, textArea.width, std::max(0, barTop - 8 - lyrics.top));
  }
  lyricsDirty_ = false;
  if (volumeDirty_) {
    const Rect panel = volumePanelRect(content);
    addDirty(panel.x - 2, panel.y - 2, panel.width + 4, panel.height + 4);
  }

  // The periodic clean only counts windowed FAST frames (~3 minutes of 1 Hz
  // ticks). A skip already drives a HALF clean, so it resets the counter and no
  // separate clean is stacked on top of it; the clean also never shares a frame
  // with a volume step or a visible overlay.
  const bool cleanDue = fastRefreshes_ >= kRefreshCleanupInterval && !fullDirty_ && !volumeDirty_ && !volumeOverlay_;
  const bool halfFrame = trackDirty_ || orientationDirty_ || cleanDue;
  const bool fullFrame = halfFrame || fullDirty_ || !haveDirty;
  // Captured for the log below, before the flags are consumed.
#if CROSSMUX_DEBUG_SERIAL_COMMANDS
  const char* flushReason = trackDirty_         ? "TRACK"
                            : orientationDirty_ ? "ROTATE"
                            : cleanDue          ? "CLEAN"
                            : fullDirty_        ? "FULL"
                            : !haveDirty        ? "NODIRTY"
                                                : "WINDOW";
#endif
  fullDirty_ = false;
  progressDirty_ = false;
  volumeDirty_ = false;
  trackDirty_ = false;
  orientationDirty_ = false;

  if (halfFrame) {
    fastRefreshes_ = 0;
    renderer.promoteNextRefresh(HalDisplay::HALF_REFRESH);
  } else {
    ++fastRefreshes_;
  }

#if CROSSMUX_DEBUG_SERIAL_COMMANDS
  // Proves the panel work done for a given frame: a 1 Hz progress tick must be a
  // window that stays clear of the artwork, a track change must be full HALF.
  const bool coverHit = coverMode && !fullFrame && dirty.x < coverRect.x + coverRect.width &&
                        coverRect.x < dirty.x + dirty.width && dirty.y < coverRect.y + coverRect.height &&
                        coverRect.y < dirty.y + dirty.height;
  const char* refreshMode = halfFrame ? "HALF" : (fullFrame ? "FAST" : "WINDOW");
  LOG_INF("MUS", "Flush mode=%s reason=%s rect=%d,%d,%dx%d cover=%d coverHit=%d coverPainted=%d", refreshMode,
          flushReason, fullFrame ? 0 : dirty.x, fullFrame ? 0 : dirty.y, fullFrame ? 0 : dirty.width,
          fullFrame ? 0 : dirty.height, coverMode ? 1 : 0, coverHit ? 1 : 0, coverSourceChanged ? 1 : 0);
#endif
  if (fullFrame) {
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  } else {
    renderer.displayWindow(dirty.x, dirty.y, dirty.width, dirty.height);
  }
}

Rect MusicPlayerActivity::volumePanelRect(const Rect& content) {
  const int panelWidth = std::max(260, content.width * 2 / 3);
  const int panelHeight = 132;
  const int panelX = content.x + (content.width - panelWidth) / 2;
  const int panelY = content.y + (content.height - panelHeight) / 2;
  return Rect{panelX, panelY, panelWidth, panelHeight};
}

void MusicPlayerActivity::captureCoverTile(const Rect& rect) {
  const size_t bytes = renderer.getRegionByteSize(rect.x, rect.y, rect.width, rect.height);
  if (bytes == 0) {
    coverTileValid_ = false;
    return;
  }
  if (!coverTile_ || coverTileBytes_ != bytes) {
    coverTile_ = makeUniqueNoThrow<uint8_t[]>(bytes);
    coverTileBytes_ = coverTile_ ? bytes : 0;
    if (!coverTile_) LOG_ERR("MUS", "Cover snapshot allocation failed (%u bytes)", static_cast<unsigned>(bytes));
  }
  coverTileRect_ = rect;
  coverTileValid_ = coverTile_ && renderer.copyRegionToBuffer(rect.x, rect.y, rect.width, rect.height, coverTile_.get(),
                                                              coverTileBytes_);
}

MusicPlayerActivity::CoverLayout MusicPlayerActivity::measureCoverLayout(const Rect& content) {
  // Match the 8 px granularity the panel uses for windowed updates, so the cover
  // border never lands mid-byte.
  const int side = std::min(content.width, content.height * 45 / 100) & ~7;
  // Portrait (the default on the 480x800 panel): there is no room for a second
  // column, so the cover goes above the text. It can only grow while the text
  // block below it keeps enough height for two title lines, status, bar and times.
  if (content.height > content.width) {
    if (side < 120 || content.height - kCoverTopGap - side < 220) return CoverLayout{};
    return CoverLayout{side, true};
  }
  // Landscape: cover on the left, text in the remaining column. Below these
  // limits the text column would be too narrow for the title and status line,
  // so fall back to the full-width centred layout instead of leaving a hole.
  if (content.width < 560 || content.height < 240) return CoverLayout{};
  const int columnSide = std::min(content.height * 62 / 100, content.width * 30 / 100) & ~7;
  if (columnSide < 120 || content.width - columnSide - 24 < 320) return CoverLayout{};
  return CoverLayout{columnSide, false};
}

void MusicPlayerActivity::drawVolumeOverlay(const Rect& content) {
  char percent[8] = {};
  snprintf(percent, sizeof(percent), "%u%%", static_cast<unsigned>(MUSIC_SESSION.volume()));
  const char* title = tr(STR_MUSIC_VOLUME);

  // Same rectangle source as the windowed refresh, so drawing and hit geometry
  // can never drift apart.
  const Rect panel = volumePanelRect(content);

  renderer.fillRect(panel.x, panel.y, panel.width, panel.height, false);
  renderer.drawRect(panel.x, panel.y, panel.width, panel.height, true);
  renderer.drawRect(panel.x + 2, panel.y + 2, panel.width - 4, panel.height - 4, true);

  drawCenteredLine(renderer, panel, UI_12_FONT_ID, panel.y + 16, title);

  const int barHeight = GUI.measureProgressBarHeight(renderer, 12, false);
  GUI.drawProgressBar(renderer, Rect{panel.x + 24, panel.y + 56, panel.width - 48, barHeight}, MUSIC_SESSION.volume(),
                      100, false);
  drawCenteredLine(renderer, panel, SMALL_FONT_ID, panel.y + 56 + barHeight + 8, percent);
}

#endif  // CROSSPOINT_CAP_MUSIC_PLAYER
