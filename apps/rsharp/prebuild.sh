#!/bin/sh
# Run by env/build.sh (inside the container) before abld: put the app icon,
# data/rsharp-icon.svg, into build/icon/rsharp_aif.mif (the SVG as it is,
# as for rSSH).
set -e
cd "$(dirname "$0")"
mkdir -p build/icon
mifconv build/icon/rsharp_aif.mif /c32,8 data/rsharp-icon.svg >/dev/null
