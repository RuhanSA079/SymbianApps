#!/bin/sh
# Fetch cJSON (MIT) into apps/common/cjson (git-ignored). Runs on the HOST.
# GitHub publishes no digest for source archives; this SHA-256 was recorded
# from the v1.7.19 archive when it was first added.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VER=1.7.19
SHA256=7fa616e3046edfa7a28a32d5f9eacfd23f92900fe1f8ccd988c1662f30454562
URL=https://github.com/DaveGamble/cJSON/archive/refs/tags/v$VER.tar.gz

DL="$ROOT/downloads/cjson"
mkdir -p "$DL"
cd "$DL"
[ -s cJSON-$VER.tar.gz ] || curl -fsSL -o cJSON-$VER.tar.gz "$URL"
echo "$SHA256  cJSON-$VER.tar.gz" | sha256sum -c -

DEST="$ROOT/apps/common/cjson"
rm -rf "$DEST" && mkdir -p "$DEST"
tar -xzf cJSON-$VER.tar.gz -C "$DEST" --strip-components=1 \
    cJSON-$VER/cJSON.c cJSON-$VER/cJSON.h cJSON-$VER/LICENSE
echo "==> cJSON $VER in apps/common/cjson"
