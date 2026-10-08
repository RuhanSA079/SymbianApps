#!/bin/sh
# Build release packages of one app for each Symbian generation. Runs on the
# HOST.
#
#   env/build-release.sh apps/rssh                 both SDKs (the default)
#   env/build-release.sh apps/rssh symbian3        just one
#
# SDKs (each installed under sdk/<name>):
#   symbian3   Symbian^3 / Anna / Belle (Nokia E7, N8, C7, ...)  -> *-symbian3.sisx
#   s60v31     S60 3rd Edition FP1 and later (Nokia E90, ...)     -> *-s60v3.sisx
#
# Output: out/release/<pkgname>-<version>-<platform>.sisx and SHA256SUMS.
# The version comes from the package header (#{"Name"},(UID),major,minor,build).
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
APP=${1:?usage: env/build-release.sh apps/<name> [symbian3] [s60v31]}
APP=${APP%/}
shift
SDKS=${*:-symbian3 s60v31}
[ -d "$APP/group" ] || { echo "$APP: no group/ directory" >&2; exit 1; }

label() {
    case $1 in
        symbian3) echo symbian3 ;;
        s60v31) echo s60v3 ;;
        *) echo "$1" ;;
    esac
}

# the package directory env/build.sh uses for an SDK
pkgdir() {
    if [ "$1" = symbian3 ]; then echo "$APP/sis"; else echo "$APP/sis/$1"; fi
}

REL=out/release
mkdir -p "$REL"
made=""
for sdk in $SDKS; do
    [ -d "sdk/$sdk/epoc32" ] || { echo "sdk/$sdk is not installed" >&2; exit 1; }
    dir=$(pkgdir "$sdk")
    ls "$dir"/*.pkg >/dev/null 2>&1 || { echo "$APP: no packages in $dir for $sdk" >&2; exit 1; }
    echo "==== $APP for $sdk"
    log="out/release/.build-$sdk.log"
    if [ "$sdk" = symbian3 ]; then
        ./sym sh env/build.sh "$APP" > "$log" 2>&1 || { tail -25 "$log"; echo "FAILED ($log)" >&2; exit 1; }
    else
        SYM_SDK=$sdk ./sym sh env/build.sh "$APP" > "$log" 2>&1 || { tail -25 "$log"; echo "FAILED ($log)" >&2; exit 1; }
    fi
    for pkg in "$dir"/*.pkg; do
        name=$(basename "$pkg" .pkg)
        [ "$sdk" = symbian3 ] && built="out/$name.sisx" || built="out/${name}_$sdk.sisx"
        ver=$(sed -n 's/^#{[^}]*},([^)]*),\([0-9]*\),\([0-9]*\),\([0-9]*\).*/\1.\2.\3/p' "$pkg" | head -1)
        dest="$REL/$name-${ver:-0.0.0}-$(label "$sdk").sisx"
        cp "$built" "$dest"
        made="$made $dest"
        echo "  -> $dest"
    done
    rm -f "$log"
done

(cd "$REL" && sha256sum *.sisx > SHA256SUMS)
printf '\nRelease files:\n'
for f in $made; do
    printf '  %s (%s bytes)\n' "$f" "$(stat -c %s "$f")"
done
echo "  $REL/SHA256SUMS"
