#!/bin/sh
# Run by env/build.sh (inside the container) before abld: build NetSurf and
# its libraries as ns_*.lib, make the app icon, and stage
# the resources + package list.
# Needs env/fetch-netsurf.sh, env/fetch-expat.sh, env/fetch-imagelibs.sh and
# env/netsurf-hostgen.sh
# to have run on the host first.
set -e
cd "$(dirname "$0")"
[ -f build/host/compile.json ] || { echo "run env/netsurf-hostgen.sh on the host first" >&2; exit 1; }
python3 tools/gen-libmk.py
make -C build/sym -j"$(nproc)" install
# app icon: data/netsurf-svgrepo-com.svg (from SVG Repo), stored in the MIF
# as it is (GnuPoc's mifconv does not encode SVG to binary SVG-T; the phone
# and EKA2L1 both read plain SVG)
mkdir -p build/icon
mifconv build/icon/netsurf_aif.mif /c32,8 data/netsurf-svgrepo-com.svg >/dev/null
python3 tools/gen-pkg.py
