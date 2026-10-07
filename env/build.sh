#!/bin/sh
# Build, package and self-sign one app. Runs INSIDE the container:
#   ./sym env/build.sh apps/hello [udeb|urel]
# Output: out/<pkgname>.sisx (signed with keys/selfsigned.*; created on first use)
set -e
APP=${1:?usage: env/build.sh apps/<name> [urel|udeb]}
CFG=${2:-urel}
cd "/work/$APP/group"
bldmake bldfiles
# abld does not notice a compiler switch (GCCE_BIN), so clean when it changes.
. /opt/symbian/gnupoc/gnupoc-common.sh
CC_ID=$(arm-none-symbianelf-gcc --version | head -1)
STAMP=.gcce-compiler
if [ "$(cat $STAMP 2>/dev/null)" != "$CC_ID" ]; then
    abld reallyclean gcce "$CFG" >/dev/null 2>&1 || true
    echo "$CC_ID" > $STAMP
fi
echo "compiler: $CC_ID"
# GnuPoc's abld exits 0 even when make fails, so check its output; otherwise
# a stale binary from an earlier build would be packaged.
LOG=$(mktemp)
abld build gcce "$CFG" 2>&1 | tee "$LOG"
if grep -qE '^make(\[[0-9]+\])?: \*\*\* |: error: |^\* RCOMP failed|undefined reference' "$LOG"; then
    echo "==> BUILD FAILED (see errors above)" >&2
    rm -f "$LOG"
    exit 1
fi
rm -f "$LOG"
[ -f /work/keys/selfsigned.key ] || sh /work/env/make-cert.sh
mkdir -p /work/out
cd ../sis
for pkg in *.pkg; do
    name=${pkg%.pkg}
    # GnuPoc makesis rejects output names shorter than 8 chars, hence the suffix.
    makesis "$pkg" "/work/out/${name}_unsigned.sis"
    signsis "/work/out/${name}_unsigned.sis" "/work/out/$name.sisx" \
        /work/keys/selfsigned.cer /work/keys/selfsigned.key
    rm "/work/out/${name}_unsigned.sis"
    echo "==> out/$name.sisx"
done
