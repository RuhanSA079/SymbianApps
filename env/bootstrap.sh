#!/bin/sh
# Set up the whole build environment from a fresh checkout and build every
# app in apps/. Runs on the HOST; each step is skipped if already done, so it
# is safe to re-run. Needs: docker, git, curl, wget, gpg, perl, python3, md5sum.
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

for tool in docker git curl wget gpg perl python3 md5sum; do
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

step "4/6 third-party sources (PuTTY; mbedTLS, cJSON; minimp3; NetSurf, expat, libpng, libjpeg)"
if [ -f apps/rssh/putty/LICENCE ]; then
    echo "PuTTY: already fetched"
else
    env/fetch-putty-src.sh
fi
if [ -f apps/common/mbedtls/LICENSE ]; then
    echo "mbedTLS: already fetched"
else
    env/fetch-mbedtls.sh
fi
if [ -f apps/common/cjson/cJSON.c ]; then
    echo "cJSON: already fetched"
else
    env/fetch-cjson.sh
fi
if [ -f apps/common/minimp3/minimp3.h ]; then
    echo "minimp3: already fetched"
else
    env/fetch-minimp3.sh
fi
if [ -f apps/netsurf/src/netsurf/Makefile ]; then
    echo "NetSurf: already fetched"
else
    env/fetch-netsurf.sh
fi
if [ -f apps/netsurf/expat/lib/xmlparse.c ]; then
    echo "expat: already fetched"
else
    env/fetch-expat.sh
fi
if [ -f apps/netsurf/libpng/png.c ] && [ -f apps/netsurf/libjpeg/jdapimin.c ]; then
    echo "libpng, libjpeg: already fetched"
else
    env/fetch-imagelibs.sh
fi

step "5/6 generated build files"
python3 -I apps/rssh/tools/gen-mmp.py
python3 -I apps/common/tools/gen-mmp.py
# NetSurf's generated sources and file lists, from a throwaway Linux build
# of it (ubuntu:24.04 container, ~10 min)
if [ -f apps/netsurf/build/host/compile.json ]; then
    echo "NetSurf host build: already done"
else
    env/netsurf-hostgen.sh
fi

step "6/6 apps (apps/common first: other apps link its libraries)"
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
