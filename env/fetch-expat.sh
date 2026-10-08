#!/bin/sh
# Fetch expat (MIT; libdom's XML parser) into apps/netsurf/expat (git-ignored).
# Runs on the HOST (needs curl, gpg). Checked against GitHub's published
# SHA-256 and the release signature by Sebastian Pipping's key, saved from
# https://keys.openpgp.org in env/expat-release-key.asc.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VER=2.9.0
TAG=R_$(echo $VER | tr . _)
SHA256=1e6371862cc31999b368c3b89b49994f0677e1bab5f1b2b85ae3741f5d803051
FPR=3176EF7DB2367F1FCA4F306B1F9B0E909AF37285
URL=https://github.com/libexpat/libexpat/releases/download/$TAG/expat-$VER.tar.xz

DL="$ROOT/downloads/expat"
mkdir -p "$DL"
cd "$DL"
[ -s expat-$VER.tar.xz ] || curl -fsSLO "$URL"
[ -s expat-$VER.tar.xz.asc ] || curl -fsSLO "$URL.asc"
echo "$SHA256  expat-$VER.tar.xz" | sha256sum -c -
G=$(mktemp -d)
trap 'rm -rf "$G"' EXIT
gpg -q --homedir "$G" --import "$ROOT/env/expat-release-key.asc" 2>/dev/null
# Signed by a subkey: VALIDSIG ends with the primary key's fingerprint.
# (Still valid after the key expires; gpg then says EXPKEYSIG, not GOODSIG.)
gpg --homedir "$G" --status-fd 1 --verify expat-$VER.tar.xz.asc expat-$VER.tar.xz 2>/dev/null \
    | grep -q "VALIDSIG .* $FPR\$" || { echo "PGP signature check FAILED" >&2; exit 1; }
echo "signature OK (expat, $FPR)"

DEST="$ROOT/apps/netsurf/expat"
rm -rf "$DEST" && mkdir -p "$DEST"
tar -xJf expat-$VER.tar.xz -C "$DEST" --strip-components=1 \
    expat-$VER/lib expat-$VER/COPYING
echo "==> expat $VER in apps/netsurf/expat"
