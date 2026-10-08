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
# Use cJSON's wrapper functions for malloc/free/realloc (its MSVC branch) on
# Symbian too: its default allocator table otherwise holds the addresses of
# libc's imported functions, which elf2e32 cannot relocate ("Import
# relocation does not refer to code segment"), and the program then fails to
# load ("Invalid ordinal ... requested from libc.dll").
# (only the #if just before the "C2322" comment: the allocator wrappers)
sed -i '/^#if defined(_MSC_VER)$/{N;s|^#if defined(_MSC_VER)\n\(/\* work around MSVC error C2322\)|#if defined(_MSC_VER) \|\| defined(__SYMBIAN32__)\n\1|}' "$DEST/cJSON.c"
grep -q 'defined(_MSC_VER) || defined(__SYMBIAN32__)' "$DEST/cJSON.c" \
    || { echo "cJSON.c: allocator patch did not apply" >&2; exit 1; }
echo "==> cJSON $VER in apps/common/cjson"
