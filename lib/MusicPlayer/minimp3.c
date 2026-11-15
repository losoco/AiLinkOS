// minimp3 (https://github.com/lieff/minimp3) — CC0, single-file MP3 decoder.
// Kept as a C translation unit so the C++ side only sees the declarations.
// Boards without the music player compile this file to nothing.
#if CROSSPOINT_CAP_MUSIC_PLAYER

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD
#include "minimp3.h"

#endif  // CROSSPOINT_CAP_MUSIC_PLAYER
