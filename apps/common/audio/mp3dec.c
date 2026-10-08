/* mp3dec.c: minimp3's implementation (minimp3/minimp3.h, CC0), MP3 only.
 * Built by apps/common/prebuild.sh as rsym_mp3.lib, with VFP code: MP3
 * synthesis is floating point, far too slow with the soft-float library on
 * an ARM11. */
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD
#include "minimp3.h"
