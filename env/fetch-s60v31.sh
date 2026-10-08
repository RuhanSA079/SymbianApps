#!/bin/sh
# Download the S60 3rd Edition FP1 SDK (Symbian OS 9.2; Nokia E90, N95, E71,
# ...) and Open C (P.I.P.S.) for it into downloads/s60v31/. Runs on the HOST.
# Then: ./sym sh env/install-sdk-s60v31.sh
#
# Sources (archive.org "symbian_dev" collection; single files are taken out
# of its big zips, which have no per-file digests, so these SHA-256s were
# recorded from the first download):
#   s60v3.1_SDK.zip            Nokia's S60_SDK_3.1_CPP_v1.0.1 installer files
#   OpenC_3_0_SDK_plugin.zip   Open C headers + libraries (epoc32 tree, 2009)
#   PythonForS60_2.0.0.tar.gz  for PyS60Dependencies/pips.sis: Nokia's
#                              Symbian-Signed P.I.P.S. runtime for phones that
#                              lack it (S60 3rd Ed, FP1, FP2, 5th Ed)
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DL="$ROOT/downloads/s60v31"
BASE=https://archive.org/download/symbian_dev
mkdir -p "$DL"
cd "$DL"

get() {   # name url sha256
    [ -s "$1" ] || curl -fL -o "$1" "$2"
    echo "$3  $1" | sha256sum -c -
}
get s60v3.1_SDK.zip "$BASE/SDK_S60.zip/SDK_S60%2Fs60v3.1_SDK.zip" \
    cd3950edb7b86abff6cd84421bcb1cb3e99ae7b1a63d02a2ce1aa2ed6e11ff24
get OpenC_3_0_SDK_plugin.zip \
    "$BASE/Development_Tools.zip/Development%20Tools%2FPyS60%2F1.9.7%2FOpenC_3_0_SDK_plugin.zip" \
    35f337128230802b53ce3cc70e1a46c5fc518889a0d1a9d0d11206f0cf53235f
get PythonForS60_2.0.0.tar.gz \
    "$BASE/Development_Tools.zip/Development%20Tools%2FPyS60%2F2.0.0%2FPythonForS60_2.0.0.tar.gz" \
    5a2d118eca899353629c26cad0887255efbcafa40db96564ee9cefe8faedfa87
tar -xzf PythonForS60_2.0.0.tar.gz -O PythonForS60/PyS60Dependencies/pips.sis > pips.sis
echo "==> S60 3rd FP1 SDK, Open C and pips.sis in downloads/s60v31"
