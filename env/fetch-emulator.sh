#!/bin/sh
# Download the EKA2L1 emulator AppImage (Linux x86_64) into tools/. Runs on the HOST.
# EKA2L1 only publishes a rolling "continous" build; the SHA-256 is taken from the
# GitHub release metadata at download time (guards against a corrupt/partial download).
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$ROOT/tools"
API=https://api.github.com/repos/EKA2L1/EKA2L1/releases/tags/continous
ASSET=EKA2L1-Linux-x86_64.AppImage
META=$(wget -qO- "$API")
INFO=$(printf '%s' "$META" | python3 -Ic '
import json, sys
for a in json.load(sys.stdin)["assets"]:
    if a["name"] == sys.argv[1]:
        print(a["browser_download_url"], (a.get("digest") or "").removeprefix("sha256:"), a["updated_at"])
' "$ASSET")
set -- $INFO
URL=$1 SHA256=$2 BUILT=$3
[ -n "$URL" ] && [ -n "$SHA256" ] || { echo "could not find $ASSET in the release" >&2; exit 1; }
OUT="$ROOT/tools/EKA2L1.AppImage"
if [ -f "$OUT" ] && echo "$SHA256  $OUT" | sha256sum -c - >/dev/null 2>&1; then
    echo "EKA2L1 build $BUILT already present"
else
    wget -q --show-progress -O "$OUT.part" "$URL"
    echo "$SHA256  $OUT.part" | sha256sum -c -
    mv "$OUT.part" "$OUT"
    chmod +x "$OUT"
    echo "installed EKA2L1 build $BUILT -> tools/EKA2L1.AppImage"
fi
