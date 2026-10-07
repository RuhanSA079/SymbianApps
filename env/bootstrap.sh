#!/bin/sh
# Set up the whole build environment from a fresh checkout and build every
# app in apps/. Runs on the HOST; each step is skipped if already done, so it
# is safe to re-run. Needs: docker, wget, gpg, perl, python3, md5sum.
#
#   env/bootstrap.sh            set up + build all apps (packages in out/)
#
# Environment:
#   SYM_IMAGE=<tag>             image name (default symbian3-env)
#   DOCKER_BUILD_FLAGS=...      e.g. --no-cache to rebuild the image fully
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
export SYM_IMAGE="${SYM_IMAGE:-symbian3-env}"

step() { printf '\n==== %s\n' "$*"; }

for tool in docker wget gpg perl python3 md5sum; do
    command -v $tool >/dev/null || { echo "missing tool: $tool" >&2; exit 1; }
done

step "1/6 Symbian^3 SDK download (866 MB, archive.org)"
env/fetch.sh

step "2/6 build environment image $SYM_IMAGE (first time: ~30-40 min, mostly GCC)"
# shellcheck disable=SC2086
docker build $DOCKER_BUILD_FLAGS -t "$SYM_IMAGE" env/

step "3/6 SDK install into sdk/symbian3"
mkdir -p sdk out keys                 # before docker mounts them (else root-owned)
if [ -d sdk/symbian3/epoc32 ]; then
    echo "already installed"
else
    ./sym sh env/install-sdk.sh
fi

step "4/6 PuTTY source for rSSH"
if [ -f apps/rssh/putty/LICENCE ]; then
    echo "already fetched"
else
    env/fetch-putty-src.sh
fi

step "5/6 generated build files"
python3 -I apps/rssh/tools/gen-mmp.py

step "6/6 apps"
for bld in apps/*/group/bld.inf; do
    app=$(dirname "$(dirname "$bld")")
    echo "--- $app"
    ./sym sh env/build.sh "$app" > "$app.build.log" 2>&1 \
        || { tail -20 "$app.build.log"; echo "FAILED: $app (log: $app.build.log)" >&2; exit 1; }
    grep '^==> ' "$app.build.log"
    rm -f "$app.build.log"
done

printf '\nDone. Signed packages are in out/:\n'
ls -1 out/*.sisx
