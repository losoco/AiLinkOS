#pragma once

#if CROSSPOINT_CAP_MUSIC_PLAYER

#include <string>

namespace MusicCoverLoader {

// Returns the path of a cached 1-bit BMP sized for a `slot` x `slot` cover,
// generating it from the track's embedded ID3 APIC (falling back to a same-name
// or album image next to the file) on a miss. Returns an empty string when no
// usable cover exists; failures never propagate as errors.
//
// Generation borrows the framebuffer through GfxRenderer::FrameBufferLoan, so
// callers must invoke this while no drawing is in progress (before clearScreen).
std::string ensureCover(const std::string& trackPath, int slot);

// Deletes the oldest cache entries once the cover directory exceeds its cap.
// Called when the library is scanned, not on every screen entry.
void pruneCache();

}  // namespace MusicCoverLoader

#endif  // CROSSPOINT_CAP_MUSIC_PLAYER
