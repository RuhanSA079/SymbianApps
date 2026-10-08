#!/bin/sh
# Fetch mbedTLS (LTS) into apps/common/mbedtls (git-ignored). Runs on the HOST.
# The SHA-256 is the digest GitHub publishes for the release asset.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VER=4.1.1
SHA256=3359a349e23db3d5536fcee032ae7b2ecbfc08972fab643089b5cbf2a375c98c
URL=https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-$VER/mbedtls-$VER.tar.bz2

DL="$ROOT/downloads/mbedtls"
mkdir -p "$DL"
cd "$DL"
[ -s mbedtls-$VER.tar.bz2 ] || wget -q "$URL"
echo "$SHA256  mbedtls-$VER.tar.bz2" | sha256sum -c -

DEST="$ROOT/apps/common/mbedtls"
rm -rf "$DEST" && mkdir -p "$DEST"
tar -xjf mbedtls-$VER.tar.bz2 -C "$DEST" --strip-components=1
for p in "$ROOT"/apps/common/patches/*.patch; do
    [ -e "$p" ] || continue
    (cd "$DEST" && patch -s -p1 < "$p") && echo "applied $(basename "$p")"
done
echo "==> mbedTLS $VER in apps/common/mbedtls"
