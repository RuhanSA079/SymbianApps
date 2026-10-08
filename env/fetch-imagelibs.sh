#!/bin/sh
# Fetch libpng and IJG libjpeg (NetSurf's PNG and JPEG decoders) into
# apps/netsurf/libpng and apps/netsurf/libjpeg (git-ignored). Runs on the HOST.
#
# libpng 1.6.59: SourceForge publishes no signature for this release. The
# SHA-256 below was recorded after checking that the tarball is identical,
# file for file, to the v1.6.59 git tag, which is GPG-signed by the
# maintainer (Cosmin Truta; GitHub shows it as verified).
# IJG jpeg 9f: IJG publishes no signatures or digests; this SHA-256 was
# recorded from https://www.ijg.org/files/ when it was first added.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PNG_VER=1.6.59
PNG_SHA256=d80dd2a38a37f803cb9b6ac7b14bd6e74ddc3b654780a8380bdf93523fdb4389
JPEG_VER=9f
JPEG_SHA256=04705c110cb2469caa79fb71fba3d7bf834914706e9641a4589485c1f832565b

mkdir -p "$ROOT/downloads/libpng" "$ROOT/downloads/libjpeg"
cd "$ROOT/downloads/libpng"
[ -s libpng-$PNG_VER.tar.xz ] || curl -fsSL -o libpng-$PNG_VER.tar.xz \
    "https://download.sourceforge.net/libpng/libpng-$PNG_VER.tar.xz"
echo "$PNG_SHA256  libpng-$PNG_VER.tar.xz" | sha256sum -c -
DEST="$ROOT/apps/netsurf/libpng"
rm -rf "$DEST" && mkdir -p "$DEST"
tar -xJf libpng-$PNG_VER.tar.xz -C "$DEST" --strip-components=1
# the configuration a default build uses
cp "$DEST/scripts/pnglibconf.h.prebuilt" "$DEST/pnglibconf.h"
echo "==> libpng $PNG_VER in apps/netsurf/libpng"

cd "$ROOT/downloads/libjpeg"
[ -s jpegsrc.v$JPEG_VER.tar.gz ] || curl -fsSLO "https://www.ijg.org/files/jpegsrc.v$JPEG_VER.tar.gz"
echo "$JPEG_SHA256  jpegsrc.v$JPEG_VER.tar.gz" | sha256sum -c -
DEST="$ROOT/apps/netsurf/libjpeg"
rm -rf "$DEST" && mkdir -p "$DEST"
tar -xzf jpegsrc.v$JPEG_VER.tar.gz -C "$DEST" --strip-components=1
# jconfig.h for an ANSI C compiler with prototypes (what configure finds)
cp "$DEST/jconfig.txt" "$DEST/jconfig.h"
echo "==> IJG libjpeg $JPEG_VER in apps/netsurf/libjpeg"
