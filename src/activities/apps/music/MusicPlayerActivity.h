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
  // Two pages share the transport block: the info page (cover, title, format)
  // and the lyric page. The list is a separate Activity, one step further back,
  // so a short Back walks Now Playing -> Lyrics -> list when a sidecar exists
  // and Now Playing -> list otherwise.
  enum class Page : uint8_t { NowPlaying, Lyrics };
  void showPage(Page page);
  bool hasLyricsPage() const { return !lyrics_.empty(); }
  // Long Confirm rotates the page a quarter turn and remembers it in
  // SETTINGS.musicOrientation, so the next visit starts where this one ended.
  void rotateOrientation();
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
  // Lyric page: track label plus a centred window of lyric lines, the current
  // one inverted. The caption and the transport block sit at the bottom of the
  // page, so the rows own everything above them.
  void drawLyricsPage(GfxRenderer& renderer, const Rect& area, const char* title, int barTop, int trackNumber,
                      int trackCount);

  bool volumeOverlay_ = false;
  uint32_t overlayDeadlineMs_ = 0;
  // Which page the player shows. Reset on entry; tracks without a .lrc have no
  // lyric page at all, so the flag is always read through hasLyricsPage().
  Page page_ = Page::NowPlaying;
  // Panel orientation to put back when this page closes: Apps may have been
  // opened from a reader that rotated the panel to landscape.
  GfxRenderer::Orientation previousOrientation_ = GfxRenderer::Orientation::Portrait;
  int lastIndex_ = -1;
  int lastStatus_ = -1;
  int lastShownSecond_ = -1;
  bool fullDirty_ = true;
  bool progressDirty_ = false;
  bool volumeDirty_ = false;
  // A track skip replaces the artwork, labels and progress block, so it is the
  // one frame pushed as a full-screen HALF clean; everything else stays windowed.
  bool trackDirty_ = false;
  // Same treatment for a quarter turn: every element moves and the page needs a
  // clean waveform. Kept apart from trackDirty_ so the flush log stays honest.
  bool orientationDirty_ = false;
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
  // onExit()/clear(). Tracks without a sidecar always fall back to the info
  // block.
  LrcLyrics lyrics_;
  bool lyricsPending_ = true;
  int lastLyricIndex_ = -2;
  bool lyricsDirty_ = false;
};

#if CROSSMUX_DEBUG_SERIAL_COMMANDS
// Serial debug channel: make the next render() re-run the cover pipeline
// without leaving the page, so an automated run can exercise it on demand.
void musicDebugRequestCoverRecheck();
// Serial debug channel: rotate exactly as a long Confirm press would, so an
// automated run can verify the layout without a physical hold.
void musicDebugRequestRotate();
// Serial debug channel: jump to the lyric page without walking the Back key.
void musicDebugRequestLyrics();
#endif

#endif  // CROSSPOINT_CAP_MUSIC_PLAYER
