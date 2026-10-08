#!/bin/sh
# Run by env/build.sh (inside the container) before abld: put the app icon,
# data/rjellyfin-icon.svg, into build/icon/rjellyfin_aif.mif (the SVG as it
# is, as for rSSH), and copy Mozilla's root certificates from NetSurf's tree
# for https:// servers. Needs env/fetch-netsurf.sh to have run on the host.
# (The player and MP3 decoder are in apps/common/audio.)
set -e
cd "$(dirname "$0")"
CA=../netsurf/src/netsurf/resources/ca-bundle
[ -f $CA ] || { echo "run env/fetch-netsurf.sh on the host first (for $CA)" >&2; exit 1; }
mkdir -p build/icon
mifconv build/icon/rjellyfin_aif.mif /c32,8 data/rjellyfin-icon.svg >/dev/null
cp $CA build/ca-bundle
