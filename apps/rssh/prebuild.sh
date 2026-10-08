#!/bin/sh
# Run by env/build.sh (inside the container) before abld: put the app icon,
# data/ssh-svgrepo-com.svg (from SVG Repo), into build/icon/rssh_aif.mif,
# which the packages install. The SVG is stored as it is (GnuPoc's mifconv
# does not encode SVG to binary SVG-T; the phone and EKA2L1 both read plain
# SVG).
set -e
cd "$(dirname "$0")"
mkdir -p build/icon
mifconv build/icon/rssh_aif.mif /c32,8 data/ssh-svgrepo-com.svg >/dev/null
