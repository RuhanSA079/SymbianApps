#!/bin/sh
# Run by env/build.sh (inside the container) before abld: build NetSurf and
# its libraries as ns_*.lib, make the app icon from NetSurf's logo, and stage
# the resources + package list.
# Needs env/fetch-netsurf.sh, env/fetch-expat.sh, env/fetch-imagelibs.sh and
# env/netsurf-hostgen.sh
# to have run on the host first.
set -e
cd "$(dirname "$0")"
[ -f build/host/compile.json ] || { echo "run env/netsurf-hostgen.sh on the host first" >&2; exit 1; }
python3 tools/gen-libmk.py
make -C build/sym -j"$(nproc)" install
# app icon: NetSurf's globe (GTK frontend's netsurf.xpm) -> MIF
python3 tools/gen-icon.py src/netsurf/frontends/gtk/res/netsurf.xpm build/icon
(cd build/icon && mifconv netsurf_aif.mif /c24,8 netsurf.bmp >/dev/null)
python3 tools/gen-pkg.py
