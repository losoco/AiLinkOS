#pragma once

#include <LrcLyrics.h>

#include <memory>
#include <string>

#include "activities/Activity.h"

#if CROSSPOINT_CAP_MUSIC_PLAYER

// Full-screen now-playing page. All playback state lives in MusicSession; this
// activity only draws it, forwards button gestures and hosts the volume
// feedback overlay. Returning to the list keeps playback running.
//
// Wheel gestures: a short press on the two direction keys (Left/Right on this
// panel) steps the volume, while holding one past the SDK's 650 ms direction
// threshold surfaces as Up/Down and skips to the previous/next track. The two
// gestures are mutually exclusive at the input layer, so one press can never do
// both; see docs/engineering/music-player-ui-design.md §6.
class MusicPlayerActivity final : public Activity {
 public:
  MusicPlayerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("music-player", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  void onExit() override;
  bool preventAutoSleep() override;
  void prepareForSleep() override;

 private:
  // What the next render() changed, and therefore which panel area needs to be
  // pushed. The frame is always redrawn completely; only the refresh is scoped
  // to the union of the dirty rectangles.
  void openVolumeOverlay();
  void closeVolumeOverlay();
  void changeVolume(int delta);
  void drawVolumeOverlay(const Rect& content);
  static Rect volumePanelRect(const Rect& content);
  // Where the square cover goes for the current content area. `side` is 0 when
  // the area is too small to host one without squeezing the text; `stacked`
  // selects cover-above-text (narrow/portrait content) over cover-beside-text.
  struct CoverLayout {
    int side = 0;
    bool stacked = false;
  };
  static CoverLayout measureCoverLayout(const Rect& content);
  // Snapshot the freshly painted artwork out of panel memory so later frames can
  // put it back without touching the card.
  void captureCoverTile(const Rect& rect);

  // Lyrics: a standard ".lrc" sidecar next to the track. The table is reloaded
  // lazily on the next render after a track change and stays cached otherwise.
  void resetLyricsForTrack();
  void loadLyricsIfPending();
  // Draws the track label plus a centred window of lyric lines, the current one
  // inverted. `barTop` bounds the block from below (the progress bar stays put);
  // when `centerTitle` is set the label is centred between `topAnchor` (the
  // artwork's bottom edge) and the first row, and the rows are anchored to the
  // bar; otherwise the label keeps the compact info-view placement.
  void drawLyricsBlock(GfxRenderer& renderer, const Rect& textArea, int topAnchor, bool centerTitle, const char* title,
                       int barTop);

  bool volumeOverlay_ = false;
  uint32_t overlayDeadlineMs_ = 0;
  int lastIndex_ = -1;
  int lastStatus_ = -1;
  int lastShownSecond_ = -1;
  bool fullDirty_ = true;
  bool progressDirty_ = false;
  bool volumeDirty_ = false;
  // A track skip replaces the artwork, labels and progress block, so it is the
  // one frame pushed as a full-screen HALF clean; everything else stays windowed.
  bool trackDirty_ = false;
  // The cover is resolved lazily on the next render, before any drawing: its
  // generation borrows the framebuffer, which must not be in use.
  bool coverPending_ = true;
  std::string coverPath_;
  // The artwork is painted from the card once per cover, then snapshotted out of
  // panel memory. Every later frame (the 1 Hz progress tick included) replays
  // that snapshot, so a periodic refresh can neither re-read the BMP nor drive
  // the artwork pixels. ~10 KB at slot 288, freed with the activity.
  std::unique_ptr<uint8_t[]> coverTile_;
  size_t coverTileBytes_ = 0;
  Rect coverTileRect_{};
  bool coverTileValid_ = false;
  uint8_t fastRefreshes_ = 0;

  // ~2 KB table + ~4 KB text while a track with lyrics is loaded; released by
  // onExit()/clear(). preferLyrics_ is only a view preference: tracks without a
  // sidecar always fall back to the info block.
  LrcLyrics lyrics_;
  bool lyricsPending_ = true;
  bool preferLyrics_ = true;
  int lastLyricIndex_ = -2;
  bool lyricsDirty_ = false;
};

#if CROSSMUX_DEBUG_SERIAL_COMMANDS
// Serial debug channel: make the next render() re-run the cover pipeline
// without leaving the page, so an automated run can exercise it on demand.
void musicDebugRequestCoverRecheck();
#endif

#endif  // CROSSPOINT_CAP_MUSIC_PLAYER
