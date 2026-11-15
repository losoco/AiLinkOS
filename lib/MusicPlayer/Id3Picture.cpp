#include "Id3Picture.h"

#if CROSSPOINT_CAP_MUSIC_PLAYER

#include <algorithm>
#include <cstring>

namespace Id3Picture {
namespace {

constexpr uint64_t kMaxTagBytes = 16ull * 1024ull * 1024ull;
constexpr uint32_t kMaxFrameBytes = 8u * 1024u * 1024u;
constexpr size_t kStreamBufferBytes = 256;
constexpr size_t kMaxMimeBytes = 64;
constexpr size_t kMaxDescriptionBytes = 4096;
constexpr int kMaxFrames = 512;

uint32_t readBigEndian32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

bool syncSafeValid(const uint8_t* p) { return (p[0] | p[1] | p[2] | p[3]) < 0x80; }

uint32_t readSyncSafe32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0] & 0x7F) << 21) | (static_cast<uint32_t>(p[1] & 0x7F) << 14) |
         (static_cast<uint32_t>(p[2] & 0x7F) << 7) | static_cast<uint32_t>(p[3] & 0x7F);
}

// Sequential reader over the tag body. With tag-level unsynchronisation the
// inserted 0x00 after a 0xFF is dropped, so frame lengths (which the spec counts
// after unsynchronisation) line up with what this reader returns.
class TagStream {
 public:
  TagStream(HalFile& file, const uint64_t bodyStart, const uint64_t bodyEnd, const bool unsynchronised)
      : file_(file), phys_(bodyStart), bodyEnd_(bodyEnd), unsynchronised_(unsynchronised) {
    file_.seek64(bodyStart);
  }

  bool readByte(uint8_t& value) {
    if (!ensureBuffered()) return false;
    value = buffer_[position_++];
    return true;
  }

  bool read(uint8_t* dst, const size_t len) {
    size_t done = 0;
    while (done < len) {
      if (!ensureBuffered()) return false;
      const size_t chunk = std::min(len - done, length_ - position_);
      memcpy(dst + done, buffer_ + position_, chunk);
      position_ += chunk;
      done += chunk;
    }
    return true;
  }

  bool skip(uint64_t len) {
    if (len == 0) return true;
    if (!unsynchronised_) {
      const uint64_t buffered = length_ - position_;
      if (len <= buffered) {
        position_ += static_cast<size_t>(len);
        return true;
      }
      len -= buffered;
      position_ = length_ = 0;
      if (phys_ + len > bodyEnd_) return false;
      phys_ += len;
      return file_.seek64(phys_);
    }
    uint8_t scratch[32];
    while (len > 0) {
      const size_t chunk = static_cast<size_t>(std::min<uint64_t>(len, sizeof(scratch)));
      if (!read(scratch, chunk)) return false;
      len -= chunk;
    }
    return true;
  }

 private:
  size_t fill() {
    if (phys_ >= bodyEnd_) return 0;
    const size_t want = static_cast<size_t>(std::min<uint64_t>(kStreamBufferBytes, bodyEnd_ - phys_));
    const int got = file_.read(buffer_, want);
    if (got <= 0) {
      phys_ = bodyEnd_;
      return 0;
    }
    phys_ += static_cast<uint64_t>(got);
    size_t kept = 0;
    for (int i = 0; i < got; ++i) {
      const uint8_t byte = buffer_[i];
      if (unsynchronised_ && lastWasFF_ && byte == 0x00) {
        lastWasFF_ = false;
        continue;
      }
      buffer_[kept++] = byte;
      lastWasFF_ = unsynchronised_ && byte == 0xFF;
    }
    position_ = 0;
    length_ = kept;
    return kept;
  }

  bool ensureBuffered() {
    while (position_ >= length_) {
      if (phys_ >= bodyEnd_) return false;
      if (fill() == 0) return false;
    }
    return true;
  }

  HalFile& file_;
  uint64_t phys_;
  const uint64_t bodyEnd_;
  const bool unsynchronised_;
  bool lastWasFF_ = false;
  uint8_t buffer_[kStreamBufferBytes] = {};
  size_t position_ = 0;
  size_t length_ = 0;
};

struct TagHeader {
  bool v22 = false;
  bool v4 = false;
  bool unsynchronised = false;
  bool extended = false;
  uint64_t bodyEnd = 0;  // absolute file offset where the frame area ends
};

struct FrameHead {
  uint8_t id[4] = {};
  size_t idLen = 0;
  uint32_t bodyLen = 0;
  bool compressed = false;
  bool encrypted = false;
  bool grouped = false;
  bool dataLengthIndicator = false;
  bool unsynchronised = false;
};

struct FrameRef {
  int index = -1;
  int pictureType = -1;
  uint32_t dataLen = 0;
  bool png = false;
  bool valid() const { return index >= 0 && dataLen > 0; }
};

bool validFrameId(const uint8_t* id, const size_t len) {
  for (size_t i = 0; i < len; ++i) {
    const uint8_t c = id[i];
    if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) return false;
  }
  return true;
}

bool readTagHeader(HalFile& file, const uint64_t fileSize, TagHeader& tag) {
  if (fileSize < 10) return false;
  uint8_t raw[10];
  if (!file.seek64(0) || file.read(raw, sizeof(raw)) != static_cast<int>(sizeof(raw))) return false;
  if (memcmp(raw, "ID3", 3) != 0) return false;

  const uint8_t major = raw[3];
  if (major < 2 || major > 4 || raw[4] == 0xFF) return false;

  const uint8_t flags = raw[5];
  tag.v22 = major == 2;
  tag.v4 = major == 4;
  // v2.2 uses 0x40 for whole-tag compression; that would need an inflater.
  if (tag.v22 && (flags & 0x40)) return false;
  tag.unsynchronised = (flags & 0x80) != 0;
  tag.extended = (flags & 0x40) != 0;

  if (!syncSafeValid(raw + 6)) return false;
  const uint64_t size = readSyncSafe32(raw + 6);
  if (size == 0 || size > kMaxTagBytes) return false;
  tag.bodyEnd = 10ull + size;
  const uint64_t physicalEnd = tag.bodyEnd + ((tag.v4 && (flags & 0x10)) ? 10ull : 0ull);
  if (physicalEnd > fileSize) return false;
  return true;
}

bool skipExtendedHeader(TagStream& stream, const TagHeader& tag) {
  if (!tag.extended) return true;
  if (tag.v22) return false;
  uint8_t size[4];
  if (!stream.read(size, sizeof(size))) return false;
  if (tag.v4) {
    if (!syncSafeValid(size)) return false;
    const uint32_t declared = readSyncSafe32(size);
    if (declared < 4) return false;
    return stream.skip(declared - 4);
  }
  return stream.skip(readBigEndian32(size));
}

enum class FrameRead { Ok, End, Error };

FrameRead readFrameHead(TagStream& stream, const TagHeader& tag, FrameHead& head) {
  const size_t headerBytes = tag.v22 ? 6 : 10;
  uint8_t raw[10];
  const bool gotFirst = stream.readByte(raw[0]);
  if (!gotFirst) return FrameRead::End;
  if (raw[0] == 0) return FrameRead::End;  // padding
  if (!stream.read(raw + 1, headerBytes - 1)) return FrameRead::Error;

  head.idLen = tag.v22 ? 3 : 4;
  memcpy(head.id, raw, head.idLen);
  if (!validFrameId(head.id, head.idLen)) return FrameRead::Error;

  uint16_t flags = 0;
  if (tag.v22) {
    head.bodyLen = (static_cast<uint32_t>(raw[3]) << 16) | (static_cast<uint32_t>(raw[4]) << 8) | raw[5];
  } else {
    if (tag.v4) {
      if (!syncSafeValid(raw + 4)) return FrameRead::Error;
      head.bodyLen = readSyncSafe32(raw + 4);
    } else {
      head.bodyLen = readBigEndian32(raw + 4);
    }
    flags = static_cast<uint16_t>((raw[8] << 8) | raw[9]);
  }
  if (head.bodyLen == 0) return FrameRead::End;

  if (!tag.v22) {
    if (tag.v4) {
      head.grouped = (flags & 0x0040) != 0;
      head.compressed = (flags & 0x0020) != 0;
      head.encrypted = (flags & 0x0010) != 0;
      head.unsynchronised = (flags & 0x0008) != 0;
      head.dataLengthIndicator = (flags & 0x0004) != 0;
    } else {
      head.compressed = (flags & 0x0080) != 0;
      head.encrypted = (flags & 0x0040) != 0;
      head.grouped = (flags & 0x0020) != 0;
    }
  }

  if (head.grouped) {
    uint8_t group = 0;
    if (!stream.readByte(group) || head.bodyLen == 0) return FrameRead::Error;
    --head.bodyLen;
  }
  return FrameRead::Ok;
}

// Parses the APIC text fields up to the picture payload. `consumed` is kept up
// to date even on failure so the caller can skip the rest of the frame.
bool parseApicHeader(TagStream& stream, const bool v22, uint32_t& consumed, int& pictureType, bool& png,
                     uint32_t& dataLen, const uint32_t bodyLen) {
  consumed = 0;
  uint8_t encoding = 0;
  if (!stream.readByte(encoding)) return false;
  consumed = 1;

  bool jpeg = false;
  if (v22) {
    uint8_t format[3];
    if (!stream.read(format, sizeof(format))) return false;
    consumed += sizeof(format);
    if (memcmp(format, "PNG", 3) == 0) {
      png = true;
    } else if (memcmp(format, "JPG", 3) == 0) {
      jpeg = true;
    } else {
      return false;
    }
  } else {
    char mime[kMaxMimeBytes + 1] = {};
    size_t mimeLen = 0;
    for (;;) {
      uint8_t byte = 0;
      if (!stream.readByte(byte) || consumed >= kMaxMimeBytes) return false;
      ++consumed;
      if (byte == 0) break;
      mime[mimeLen++] = static_cast<char>(byte);
    }
    for (size_t i = 0; i < mimeLen; ++i) {
      if (mime[i] >= 'A' && mime[i] <= 'Z') mime[i] = static_cast<char>(mime[i] + ('a' - 'A'));
    }
    if (strstr(mime, "png") != nullptr) {
      png = true;
    } else if (strstr(mime, "jpg") != nullptr || strstr(mime, "jpeg") != nullptr) {
      jpeg = true;
    } else {
      return false;  // includes the "-->" linked-picture MIME
    }
  }

  uint8_t type = 0;
  if (!stream.readByte(type)) return false;
  ++consumed;
  pictureType = type;

  if (encoding == 1 || encoding == 2) {
    for (;;) {
      uint8_t high = 0;
      uint8_t low = 0;
      if (!stream.readByte(high) || !stream.readByte(low)) return false;
      consumed += 2;
      if (high == 0 && low == 0) break;
      if (consumed > kMaxDescriptionBytes) return false;
    }
  } else {
    for (;;) {
      uint8_t byte = 0;
      if (!stream.readByte(byte)) return false;
      ++consumed;
      if (byte == 0) break;
      if (consumed > kMaxDescriptionBytes) return false;
    }
  }

  if (consumed >= bodyLen) return false;
  dataLen = bodyLen - consumed;
  return dataLen > 0 && dataLen <= kMaxFrameBytes;
}

bool isApic(const FrameHead& head, const bool v22) {
  return v22 ? memcmp(head.id, "PIC", 3) == 0 : memcmp(head.id, "APIC", 4) == 0;
}

bool scanFrames(HalFile& file, const TagHeader& tag, FrameRef& best) {
  TagStream stream(file, 10, tag.bodyEnd, tag.unsynchronised);
  if (!skipExtendedHeader(stream, tag)) return false;

  for (int index = 0; index < kMaxFrames; ++index) {
    FrameHead head;
    const FrameRead status = readFrameHead(stream, tag, head);
    if (status != FrameRead::Ok) break;

    // Per-frame unsynchronisation (v2.4) is left unsupported: it needs a second
    // filtered view over the frame body, and no tagger in the wild emits it.
    // Such a frame is skipped like a compressed one, falling back to sidecars.
    const bool usable = isApic(head, tag.v22) && !head.compressed && !head.encrypted && !head.dataLengthIndicator &&
                        !head.unsynchronised;
    if (usable) {
      uint32_t consumed = 0;
      int pictureType = -1;
      bool png = false;
      uint32_t dataLen = 0;
      const bool parsed = parseApicHeader(stream, tag.v22, consumed, pictureType, png, dataLen, head.bodyLen);
      if (parsed && (pictureType == 3 || pictureType == 0)) {
        // Type 3 is "front cover", type 0 is "other"; only those two reach here.
        const bool better = !best.valid() || pictureType > best.pictureType ||
                            (pictureType == best.pictureType && dataLen > best.dataLen);
        if (better) best = FrameRef{index, pictureType, dataLen, png};
      }
      if (consumed > head.bodyLen) break;
      if (!stream.skip(head.bodyLen - consumed)) break;
    } else {
      if (!stream.skip(head.bodyLen)) break;
    }
  }
  return best.valid();
}

bool copyFrame(HalFile& file, const TagHeader& tag, const FrameRef& target, HalFile& out, PictureInfo& info) {
  TagStream stream(file, 10, tag.bodyEnd, tag.unsynchronised);
  if (!skipExtendedHeader(stream, tag)) return false;

  for (int index = 0; index < kMaxFrames; ++index) {
    FrameHead head;
    if (readFrameHead(stream, tag, head) != FrameRead::Ok) return false;

    if (index == target.index) {
      uint32_t consumed = 0;
      int pictureType = -1;
      bool png = false;
      uint32_t dataLen = 0;
      if (!parseApicHeader(stream, tag.v22, consumed, pictureType, png, dataLen, head.bodyLen)) return false;
      if (dataLen != target.dataLen) return false;  // parser drifted; do not write junk

      uint8_t chunk[kStreamBufferBytes];
      uint32_t remaining = dataLen;
      while (remaining > 0) {
        const size_t chunkBytes = std::min<size_t>(sizeof(chunk), remaining);
        if (!stream.read(chunk, chunkBytes)) return false;
        if (out.write(chunk, chunkBytes) != chunkBytes) return false;
        remaining -= static_cast<uint32_t>(chunkBytes);
      }
      info.length = dataLen;
      info.png = png;
      return true;
    }

    if (!stream.skip(head.bodyLen)) return false;
  }
  return false;
}

}  // namespace

bool extract(HalFile& source, const uint64_t fileSize, HalFile& out, PictureInfo& info) {
  info = PictureInfo{};

  TagHeader tag;
  if (!readTagHeader(source, fileSize, tag)) return false;

  FrameRef best;
  if (!scanFrames(source, tag, best)) return false;
  if (!copyFrame(source, tag, best, out, info)) {
    info = PictureInfo{};
    return false;
  }
  return info.valid();
}

}  // namespace Id3Picture

#endif  // CROSSPOINT_CAP_MUSIC_PLAYER
