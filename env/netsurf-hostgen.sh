#!/bin/sh
# Build NetSurf's framebuffer frontend for Linux once, in a throwaway
# Ubuntu container, to produce what the Symbian build cannot generate itself:
# gperf/perl/python output in the libraries, nsgenbind's Duktape bindings,
# the framebuffer font and images, and the Messages files. Runs on the HOST.
#
#   env/netsurf-hostgen.sh        # needs env/fetch-netsurf.sh first
#
# Results (all git-ignored):
#   apps/netsurf/build/host/tree         the built copy of apps/netsurf/src
#   apps/netsurf/build/host/build.log    full make log (Q= : every command)
#   apps/netsurf/build/host/compile.json per-component sources, -D and -I
#   apps/netsurf/gen/<component>/...     generated files, at their tree paths
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/apps/netsurf/src"
HB="$ROOT/apps/netsurf/build/host"
GEN="$ROOT/apps/netsurf/gen"
[ -d "$SRC/netsurf" ] || { echo "run env/fetch-netsurf.sh first" >&2; exit 1; }

rm -rf "$HB" "$GEN"
mkdir -p "$HB"
cp -a "$SRC" "$HB/tree"

# Same order as netsurf-all's Makefile; nsgenbind is a build-machine tool.
LIBS="buildsystem libnslog libwapcaplet libparserutils libcss libhubbub libdom
libnsbmp libnsgif librosprite libnsutils libutf8proc libnspsl libsvgtiny libnsfb"

echo "==> host build in ubuntu:24.04 (log: apps/netsurf/build/host/build.log)"
docker run --rm -v "$HB:/hb" -w /hb/tree -e LIBS="$LIBS" ubuntu:24.04 sh -c '
set -e
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq >/dev/null
apt-get install -y -qq build-essential pkg-config flex bison gperf perl python3 \
  libcurl4-openssl-dev libssl-dev libpng-dev libjpeg-dev libexpat1-dev \
  libsdl1.2-dev libfreetype-dev libxkbcommon-dev zlib1g-dev >/dev/null 2>&1
P=/hb/tree/inst-framebuffer
mkdir -p $P/include $P/lib $P/bin
export PKG_CONFIG_PATH=$P/lib/pkgconfig PATH=$PATH:$P/bin
rc=0
(
  set -e
  for l in $LIBS; do
    make install -C $l PREFIX=$P Q= DESTDIR= WARNFLAGS="-Wall -W -Wno-error" -j8
  done
  # libutf8proc installs its header one level down; NetSurf includes it flat.
  cp $P/include/libutf8proc/utf8proc.h $P/include/
  make install -C nsgenbind PREFIX=$P Q= DESTDIR= -j8
  make -C netsurf TARGET=framebuffer Q= -j8
) > /hb/build.log 2>&1 || rc=$?
chown -R '"$(id -u):$(id -g)"' /hb
exit $rc
' || { echo "==> host build FAILED; see $HB/build.log" >&2; tail -5 "$HB/build.log" >&2; exit 1; }

python3 -I "$ROOT/apps/netsurf/tools/hostgen-extract.py" "$SRC" "$HB" "$GEN"
echo "==> generated sources in apps/netsurf/gen"
