#pragma once

#if CROSSPOINT_CAP_MUSIC_PLAYER

#include <HalStorage.h>

#include <cstdint>

// Streams the album-art frame out of an ID3v2 tag without keeping the image in
// RAM. Only frame headers and the APIC text fields are parsed; the picture
// payload is copied straight to a caller-provided file so the existing
// JPEG/PNG converters can decode it afterwards.
namespace Id3Picture {

struct PictureInfo {
  uint32_t length = 0;
  bool png = false;
  bool valid() const { return length > 0; }
};

// Walks the ID3v2 tag at the start of `source` (v2.2/2.3/2.4) and copies the
// best front-cover frame into `out`: picture type 3 first, type 0 second, the
// largest payload within a type. Tag-level unsynchronisation is undone while
// copying. Compressed, encrypted, linked and 32x32 icon frames are skipped.
//
// Returns false when the file has no usable tag/frame or any structural limit
// is exceeded. `out` may then contain partial data; the caller must discard it.
bool extract(HalFile& source, uint64_t fileSize, HalFile& out, PictureInfo& info);

}  // namespace Id3Picture

#endif  // CROSSPOINT_CAP_MUSIC_PLAYER
