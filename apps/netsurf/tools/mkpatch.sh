#!/bin/sh
# Turn local edits in apps/netsurf/src/<repo> into apps/netsurf/patches/
# <NN>-<repo>-symbian.patch (diff against the commit pinned in
# env/netsurf.lock), so env/fetch-netsurf.sh reapplies them. Runs on the HOST.
#   apps/netsurf/tools/mkpatch.sh netsurf libnsfb
set -e
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
SRC="$ROOT/apps/netsurf/src"
CACHE="$ROOT/downloads/netsurf/git"
OUT="$ROOT/apps/netsurf/patches"
mkdir -p "$OUT"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
n=1
for repo in "$@"; do
    commit=$(awk -v r="$repo" '$1 == r { print $2 }' "$ROOT/env/netsurf.lock")
    [ -n "$commit" ] || { echo "$repo is not in env/netsurf.lock" >&2; exit 1; }
    mkdir -p "$T/a/$repo"
    git -C "$CACHE/$repo" archive "$commit" | tar -x -C "$T/a/$repo"
    mkdir -p "$T/b"
    ln -s "$SRC/$repo" "$T/b/$repo"
    patch="$OUT/$(printf %02d $n)-$repo-symbian.patch"
    # files that exist upstream and differ (build output and generated
    # files in the tree are not upstream files, so they are skipped)
    (cd "$T" && for f in $(git -C "$CACHE/$repo" ls-tree -r --name-only "$commit"); do
        if [ -f "b/$repo/$f" ] && ! cmp -s "a/$repo/$f" "b/$repo/$f"; then
            diff -u --label "a/$repo/$f" --label "b/$repo/$f" "a/$repo/$f" "b/$repo/$f" || true
        fi
    done) > "$patch"
    if [ -s "$patch" ]; then
        echo "wrote $(basename "$patch") ($(grep -c '^+++ ' "$patch") files)"
        n=$((n + 1))
    else
        rm -f "$patch"
        echo "$repo: no changes"
    fi
done
