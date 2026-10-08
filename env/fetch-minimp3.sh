#!/bin/sh
# Fetch minimp3 (CC0, public domain; the MP3 decoder in apps/common/audio, used
# by rInternetRadio and rJellyfin) into apps/common/minimp3 (git-ignored).
# Runs on the HOST.
# GitHub publishes no digest for source archives; this SHA-256 was recorded
# from the archive of the pinned commit when it was first added.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
COMMIT=ea99364f61c14656440e8d77e9c233ccf3124633
SHA256=5628166eb82a9bb581317918a334c317a2c0a30278bb14a20381307976768f34
URL=https://github.com/lieff/minimp3/archive/$COMMIT.tar.gz

DL="$ROOT/downloads/minimp3"
mkdir -p "$DL"
cd "$DL"
[ -s minimp3-$COMMIT.tar.gz ] || curl -fsSL -o minimp3-$COMMIT.tar.gz "$URL"
echo "$SHA256  minimp3-$COMMIT.tar.gz" | sha256sum -c -

DEST="$ROOT/apps/common/minimp3"
rm -rf "$DEST" && mkdir -p "$DEST"
tar -xzf minimp3-$COMMIT.tar.gz -C "$DEST" --strip-components=1 \
    minimp3-$COMMIT/minimp3.h minimp3-$COMMIT/LICENSE
echo "==> minimp3 $COMMIT in apps/common/minimp3"
