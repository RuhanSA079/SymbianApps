#!/bin/sh
# Fetch NetSurf and its libraries at the commits pinned in env/netsurf.lock
# into apps/netsurf/src/<name> (git-ignored). Runs on the HOST.
# Clones are cached in downloads/netsurf/git/; git commit IDs are content
# hashes, so the pinned commits are what gets built even over git://.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BASE=${NETSURF_GIT_BASE:-git://git.netsurf-browser.org}
CACHE="$ROOT/downloads/netsurf/git"
DEST="$ROOT/apps/netsurf/src"
mkdir -p "$CACHE"

grep -vE '^\s*(#|$)' "$ROOT/env/netsurf.lock" | while read name commit; do
    repo="$CACHE/$name"
    if [ ! -d "$repo/.git" ]; then
        git clone -q "$BASE/$name.git" "$repo"
    fi
    if ! git -C "$repo" cat-file -e "$commit^{commit}" 2>/dev/null; then
        git -C "$repo" fetch -q origin
    fi
    rm -rf "$DEST/$name"
    mkdir -p "$DEST/$name"
    git -C "$repo" archive "$commit" | tar -x -C "$DEST/$name"
    got=$(git -C "$repo" rev-parse "$commit^{commit}")
    [ "$got" = "$commit" ] || { echo "commit mismatch for $name" >&2; exit 1; }
    echo "$name @ $(echo $commit | cut -c1-10)"
done
for p in "$ROOT"/apps/netsurf/patches/*.patch; do
    [ -e "$p" ] || continue
    (cd "$DEST" && patch -s -p1 < "$p") && echo "applied $(basename "$p")"
done
echo "==> NetSurf sources in apps/netsurf/src"
