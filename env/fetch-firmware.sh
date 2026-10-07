#!/bin/sh
# Download + unpack Nokia E7 (RM-626) firmware for the EKA2L1 emulator.
# Runs on the HOST (needs wget, sha1sum, and unar or unrar). Nokia's firmware is copyrighted:
# it lands in downloads/firmware/ (git-ignored) and must never be committed.
#
#   env/fetch-firmware.sh classic   Belle Refresh 111.040.1511, LTA variant, SymbianLatino
#                                   "Classic" build (patched to install unsigned SIS) [default]
#   env/fetch-firmware.sh stock     uploads of 111.030.0609 + 111.040.1511, India variant
#
# Then in EKA2L1: Devices -> install wizard -> "Firmware (VPL)" -> pick the .vpl
# printed below (its .fpsx files must stay in the same folder).
set -e
VARIANT=${1:-classic}
case "$VARIANT" in
classic)
    URL="https://archive.org/download/nokia-symbian-firmware-collection-classic-belle-refresh-symbianlatino/Classic%20Nokia%20Belle%20Refresh%20-%20E7%20RM-626.rar"
    SHA1=f243328ff8879a32c03b34cc401a682313aef1fe ;;
stock)
    URL="https://archive.org/download/rm-626/RM-626.rar"
    SHA1=e8fbdf7ff3676651198e41d2e16170d20f27d20f ;;
*)
    echo "usage: $0 [classic|stock]" >&2; exit 1 ;;
esac
# Ubuntu's p7zip has no RAR decoder, so use a real RAR extractor.
if command -v unrar >/dev/null; then EXTRACT='unrar x -idq -o+ "$RAR" "$TMP/"'
elif command -v unar >/dev/null; then EXTRACT='unar -q -f -D -o "$TMP" "$RAR"'
else echo "need unar or unrar (apt install unar)" >&2; exit 1; fi

DIR="$(cd "$(dirname "$0")/.." && pwd)/downloads/firmware"
mkdir -p "$DIR"
RAR="$DIR/e7-$VARIANT.rar"
OUT="$DIR/$VARIANT"

wget -c -O "$RAR" "$URL"
echo "$SHA1  $RAR" | sha1sum -c -

if [ -d "$OUT" ]; then
    echo "$OUT already exists; delete it to re-extract"
else
    # Extract into a fresh dir first so a failed run never leaves a half-filled $OUT.
    TMP=$(mktemp -d "$DIR/.extract.XXXX")
    eval "$EXTRACT"
    mv "$TMP" "$OUT"
fi
echo "VPL file(s) for EKA2L1:"
find "$OUT" -iname '*.vpl'
