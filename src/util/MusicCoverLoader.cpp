#include "MusicCoverLoader.h"

#if CROSSPOINT_CAP_MUSIC_PLAYER

#include <Bitmap.h>
#include <HalStorage.h>
#include <Id3Picture.h>
#include <JpegToBmpConverter.h>
#include <Logging.h>
#include <PngToBmpConverter.h>
#include <WString.h>
#include <strings.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace MusicCoverLoader {
namespace {

constexpr const char* kCacheDir = "/.crosspoint/music/covers";
constexpr const char* kTempPicturePath = "/.crosspoint/music/covers/.picture";
constexpr int kMaxCachedCovers = 200;
constexpr int kPruneTarget = 150;
constexpr int kListCap = 300;

// FNV-1a over path + size + mtime + slot. Size and mtime make a replaced file
// (or a replaced cover inside it) miss naturally, so there is no invalidation
// bookkeeping beyond pruning old entries.
uint64_t coverKey(const std::string& path, const uint64_t size, const uint32_t mtime, const int slot) {
  constexpr uint64_t kOffsetBasis = 1469598103934665603ull;
  constexpr uint64_t kPrime = 1099511628211ull;
  uint64_t hash = kOffsetBasis;
  const auto mix = [&hash](const uint8_t* data, const size_t len) {
    for (size_t i = 0; i < len; ++i) {
      hash ^= data[i];
      hash *= kPrime;
    }
  };
  mix(reinterpret_cast<const uint8_t*>(path.data()), path.size());

  uint8_t tail[13] = {};
  memcpy(tail, &size, sizeof(size));
  memcpy(tail + sizeof(size), &mtime, sizeof(mtime));
  tail[12] = static_cast<uint8_t>(std::clamp(slot, 0, 255));
  mix(tail, sizeof(tail));
  return hash;
}

bool ensureCacheDir() {
  // SdFat's mkdir does not create parents, so walk the path one level at a time.
  static constexpr const char* kDirectories[] = {"/.crosspoint", "/.crosspoint/music", kCacheDir};
  for (const char* directory : kDirectories) {
    if (!Storage.ensureDirectoryExists(directory)) return false;
  }
  return true;
}

bool validCachedCover(const std::string& path, const int slot) {
  HalFile file;
  if (!Storage.openFileForRead("MCOV", path, file)) return false;
  if (file.isDirectory() || file.fileSize() == 0) return false;

  Bitmap bitmap(file);
  const BmpReaderError header = bitmap.parseHeaders();
  if (header != BmpReaderError::Ok || !bitmap.is1Bit()) {
    LOG_ERR("MCOV", "Cached cover rejected (%s, bpp=%u): %s", path.c_str(), static_cast<unsigned>(bitmap.getBpp()),
            Bitmap::errorToString(header));
    Storage.remove(path.c_str());
    return false;
  }
  LOG_INF("MCOV", "Cached cover %dx%d bpp=%u slot=%d file=%llu", bitmap.getWidth(), bitmap.getHeight(),
          static_cast<unsigned>(bitmap.getBpp()), slot, static_cast<unsigned long long>(file.fileSize64()));
  // Both converters truncate the scaled extent (a 300x300 source at 200 gives
  // 199), so allow one pixel of slack on the short side; otherwise every render
  // would throw the cache away and decode again.
  const int shortSide = std::min(bitmap.getWidth(), bitmap.getHeight());
  const int longSide = std::max(bitmap.getWidth(), bitmap.getHeight());
  if (shortSide + 1 < slot || longSide < slot) {
    LOG_ERR("MCOV", "Cached cover undersized (%dx%d) for slot %d", bitmap.getWidth(), bitmap.getHeight(), slot);
    Storage.remove(path.c_str());
    return false;
  }
  // A truncated cache file parses but cannot be drawn; drop it and regenerate.
  const uint64_t pixelBytes = static_cast<uint64_t>(bitmap.getRowBytes()) * static_cast<uint64_t>(bitmap.getHeight());
  if (static_cast<uint64_t>(bitmap.getPixelDataOffset()) + pixelBytes > file.fileSize64()) {
    LOG_ERR("MCOV", "Cached cover truncated (%llu < %llu)", static_cast<unsigned long long>(file.fileSize64()),
            static_cast<unsigned long long>(bitmap.getPixelDataOffset()) + pixelBytes);
    Storage.remove(path.c_str());
    return false;
  }
  return true;
}

bool convertToCache(const std::string& sourcePath, const bool pngPreferred, const int slot,
                    const std::string& cachePath) {
  // Try the format the tag declares first, then the other one: some taggers
  // write a wrong MIME, and the converters themselves are the checksum.
  for (int attempt = 0; attempt < 2; ++attempt) {
    const bool usePng = (attempt == 0) == pngPreferred;
    HalFile source;
    if (!Storage.openFileForRead("MCOV", sourcePath, source)) {
      LOG_ERR("MCOV", "Cannot open cover source %s", sourcePath.c_str());
      return false;
    }
    HalFile target;
    if (!Storage.openFileForWrite("MCOV", cachePath, target)) {
      LOG_ERR("MCOV", "Cannot create cover cache file %s", cachePath.c_str());
      return false;
    }
    const bool converted = usePng ? PngToBmpConverter::pngFileTo1BitBmpStreamWithSize(source, target, slot, slot)
                                  : JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(source, target, slot, slot);
    target.flush();
    LOG_INF("MCOV", "Convert %s -> %dx%d %s", usePng ? "PNG" : "JPEG", slot, slot, converted ? "ok" : "failed");
    if (converted) return true;
  }
  return false;
}

bool extractPicture(const std::string& trackPath, Id3Picture::PictureInfo& info) {
  HalFile source;
  if (!Storage.openFileForRead("MCOV", trackPath, source)) return false;
  if (source.isDirectory()) return false;
  const uint64_t size = source.fileSize64();

  bool extracted = false;
  {
    HalFile out;
    if (!Storage.openFileForWrite("MCOV", kTempPicturePath, out)) return false;
    extracted = Id3Picture::extract(source, size, out, info);
    out.flush();
  }
  if (!extracted) {
    Storage.remove(kTempPicturePath);
    info = Id3Picture::PictureInfo{};
  }
  return extracted;
}

bool hasPngExtension(const std::string& path) {
  const size_t dot = path.find_last_of('.');
  return dot != std::string::npos && strcasecmp(path.c_str() + dot, ".png") == 0;
}

std::string findSidecar(const std::string& trackPath) {
  const size_t slash = trackPath.find_last_of('/');
  const size_t dot = trackPath.find_last_of('.');
  const std::string base = (dot != std::string::npos && (slash == std::string::npos || dot > slash + 1))
                               ? trackPath.substr(0, dot)
                               : trackPath;

  static constexpr const char* kExtensions[] = {".jpg", ".JPG", ".jpeg", ".JPEG", ".png", ".PNG"};
  for (const char* extension : kExtensions) {
    const std::string candidate = base + extension;
    if (Storage.exists(candidate.c_str())) return candidate;
  }

  // Whole-album image next to the tracks, in the order players commonly use.
  const std::string directory = slash == std::string::npos ? std::string() : trackPath.substr(0, slash + 1);
  static constexpr const char* kAlbumNames[] = {"cover", "folder", "front"};
  for (const char* name : kAlbumNames) {
    for (const char* extension : kExtensions) {
      const std::string candidate = directory + name + extension;
      if (Storage.exists(candidate.c_str())) return candidate;
    }
  }
  return "";
}

}  // namespace

std::string ensureCover(const std::string& trackPath, const int slot) {
  if (slot <= 0 || trackPath.empty() || !Storage.ready()) {
    LOG_ERR("MCOV", "Skipped: slot=%d empty=%d ready=%d", slot, trackPath.empty() ? 1 : 0, Storage.ready() ? 1 : 0);
    return "";
  }

  uint64_t size = 0;
  uint32_t mtime = 0;
  {
    HalFile track;
    if (!Storage.openFileForRead("MCOV", trackPath, track)) {
      LOG_ERR("MCOV", "Cannot open track %s", trackPath.c_str());
      return "";
    }
    if (track.isDirectory()) return "";
    size = track.fileSize64();
    mtime = track.modificationTime();
  }

  char name[24] = {};
  snprintf(name, sizeof(name), "%016llx.bmp", static_cast<unsigned long long>(coverKey(trackPath, size, mtime, slot)));
  const std::string cachePath = std::string(kCacheDir) + "/" + name;
  LOG_INF("MCOV", "ensure %s size=%llu mtime=%lu slot=%d -> %s", trackPath.c_str(),
          static_cast<unsigned long long>(size), static_cast<unsigned long>(mtime), slot, name);
  if (validCachedCover(cachePath, slot)) return cachePath;

  if (!ensureCacheDir()) {
    LOG_ERR("MCOV", "Cannot create %s", kCacheDir);
    return "";
  }

  Id3Picture::PictureInfo info;
  if (extractPicture(trackPath, info)) {
    LOG_INF("MCOV", "APIC extracted len=%lu png=%d", static_cast<unsigned long>(info.length), info.png ? 1 : 0);
    const bool converted = convertToCache(kTempPicturePath, info.png, slot, cachePath);
    Storage.remove(kTempPicturePath);
    if (converted && validCachedCover(cachePath, slot)) return cachePath;
    Storage.remove(cachePath.c_str());
  } else {
    LOG_INF("MCOV", "No usable APIC in %s", trackPath.c_str());
  }

  const std::string sidecar = findSidecar(trackPath);
  if (!sidecar.empty()) {
    if (convertToCache(sidecar, hasPngExtension(sidecar), slot, cachePath) && validCachedCover(cachePath, slot)) {
      return cachePath;
    }
    Storage.remove(cachePath.c_str());
  }

  LOG_DBG("MCOV", "No usable cover for %s", trackPath.c_str());
  return "";
}

void pruneCache() {
  if (!Storage.ready()) return;
  // Left behind only by a reset in the middle of an extraction; harmless to drop.
  Storage.remove(kTempPicturePath);

  std::vector<String> names = Storage.listFiles(kCacheDir, kListCap);
  int coverCount = 0;
  for (const String& name : names) {
    if (name.endsWith(".bmp")) ++coverCount;
  }
  if (coverCount <= kMaxCachedCovers) return;

  // Parallel index/mtime arrays keep the String vector as the only path owner.
  std::vector<uint16_t> indexes;
  std::vector<uint32_t> modified;
  indexes.reserve(static_cast<size_t>(coverCount));
  modified.reserve(static_cast<size_t>(coverCount));

  for (size_t i = 0; i < names.size(); ++i) {
    if (!names[i].endsWith(".bmp")) continue;
    const std::string path = std::string(kCacheDir) + "/" + names[i].c_str();
    HalFile file;
    const uint32_t mtime = Storage.openFileForRead("MCOV", path, file) ? file.modificationTime() : 0;
    indexes.push_back(static_cast<uint16_t>(i));
    modified.push_back(mtime);
  }

  size_t remaining = indexes.size();
  while (remaining > static_cast<size_t>(kPruneTarget)) {
    size_t oldest = 0;
    for (size_t i = 1; i < indexes.size(); ++i) {
      if (modified[i] < modified[oldest]) oldest = i;
    }
    const std::string path = std::string(kCacheDir) + "/" + names[indexes[oldest]].c_str();
    Storage.remove(path.c_str());
    modified[oldest] = 0xFFFFFFFFu;
    --remaining;
  }
  LOG_INF("MCOV", "Pruned cover cache to %d entries", kPruneTarget);
}

}  // namespace MusicCoverLoader

#endif  // CROSSPOINT_CAP_MUSIC_PLAYER
