#!/bin/sh
# Run by env/build.sh (inside the container) before abld: build the MP3
# decoder (audio/mp3dec.c, minimp3) as rsym_mp3.lib. Not by abld: its
# synthesis is floating point and needs VFP instructions, and abld always
# adds -msoft-float after an .mmp's own options. Otherwise the flags are
# abld's GCCE urel ones (as in apps/netsurf/tools/gen-libmk.py), in ARM
# rather than Thumb code. softfp keeps the soft-float calling convention, so
# the library links with everything else. Needs env/fetch-minimp3.sh to have
# run on the host.
set -e
cd "$(dirname "$0")"
[ -f minimp3/minimp3.h ] || { echo "run env/fetch-minimp3.sh on the host first" >&2; exit 1; }
EPOC=${EPOCROOT}epoc32
BIN=${GCCE_BIN:-/opt/symbian/gcc-14.2/bin}
mkdir -p build/obj
$BIN/arm-none-symbianelf-gcc -O2 -fno-unit-at-a-time -march=armv5t -mapcs \
    -mthumb-interwork -marm -mfloat-abi=softfp -mfpu=vfp -nostdinc -pipe \
    -Wno-unknown-pragmas -D__MARM_INTERWORK__ -DNDEBUG -D_UNICODE -D__GCCE__ \
    -D__SYMBIAN32__ -D__EPOC32__ -D__MARM__ -D__EABI__ -D__MARM_ARMV5__ \
    -D__SUPPORT_CPP_EXCEPTIONS__ \
    "-D__PRODUCT_INCLUDE__=\"$EPOC/include/variant/symbian_os.hrh\"" \
    -include $EPOC/include/gcce/gcce.h -std=gnu99 \
    -Iminimp3 -I$EPOC/include/stdapis -I$EPOC/include -I$EPOC/include/variant \
    -isystem "$($BIN/arm-none-symbianelf-gcc -print-file-name=include)" \
    -c audio/mp3dec.c -o build/obj/mp3dec.o
rm -f build/rsym_mp3.lib
$BIN/arm-none-symbianelf-ar cr build/rsym_mp3.lib build/obj/mp3dec.o
cp build/rsym_mp3.lib $EPOC/release/armv5/urel/
