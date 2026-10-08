#!/bin/sh
# Throwaway Jellyfin server for testing rJellyfin. Runs on the HOST.
#   env/test-jellyfin.sh start    # http://127.0.0.1:8096, user "test", password "test"
#   env/test-jellyfin.sh stop
#   env/test-jellyfin.sh reset    # stop and delete its data (downloads/test-jellyfin)
# It listens only on 127.0.0.1 (the EKA2L1 emulator reaches it there), as
# its password is no secret. For a phone on the network, start it with
# TEST_JELLYFIN_BIND=0.0.0.0.
# On first start it generates a small music library (an album of test tones:
# FLAC, which the server converts to MP3 for the phone, and one MP3 as is)
# and goes through Jellyfin's setup wizard by its API.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
NAME=symbian-test-jellyfin
IMAGE=jellyfin/jellyfin:10.10.7
DIR="$ROOT/downloads/test-jellyfin"
BIND=${TEST_JELLYFIN_BIND:-127.0.0.1}
URL=http://127.0.0.1:8096
AUTH='MediaBrowser Client="test-jellyfin.sh", Device="host", DeviceId="test-jellyfin-sh", Version="1"'
ME="$(id -u):$(id -g)"

ffmpeg() {
    docker run --rm --user "$ME" --entrypoint /usr/lib/jellyfin-ffmpeg/ffmpeg \
        -v "$DIR/media:/media" "$IMAGE" -hide_banner -loglevel error -y "$@"
}

make_media() {
    d="/media/music/rJellyfin Test Tones"
    mkdir -p "$DIR/media/music/rJellyfin Test Tones"
    n=1
    for f in 440 660 880; do
        ext=flac
        [ $n = 3 ] && ext=mp3
        ffmpeg -f lavfi -i "sine=frequency=$f:duration=25" -ac 2 -ar 44100 \
            -metadata title="Tone $f Hz" -metadata artist="rJellyfin Test" \
            -metadata album_artist="rJellyfin Test" -metadata album="rJellyfin Test Tones" \
            -metadata track=$n -metadata date=2026 \
            $( [ $ext = mp3 ] && echo "-b:a 128k" ) "$d/0$n - Tone $f Hz.$ext"
        n=$((n + 1))
    done
    # a cover, for the artwork
    ffmpeg -f lavfi -i "testsrc=size=300x300:rate=1" -frames:v 1 "$d/folder.jpg"
}

wait_up() {
    i=0
    until curl -fs "$URL/System/Info/Public" >/dev/null 2>&1; do
        i=$((i + 1))
        [ $i -gt 90 ] && { echo "Jellyfin did not start" >&2; docker logs --tail 20 $NAME >&2; exit 1; }
        sleep 1
    done
}

post() {    # path json [token]
    curl -fsS -X POST "$URL$1" -H "Content-Type: application/json" \
        -H "Authorization: $AUTH${3:+, Token=\"$3\"}" -d "$2"
}

setup() {
    if curl -fs "$URL/System/Info/Public" | grep -q '"StartupWizardCompleted":true'; then
        return
    fi
    echo "first start: setup wizard, user test/test, Music library"
    post /Startup/Configuration '{"UICulture":"en-US","MetadataCountryCode":"US","PreferredMetadataLanguage":"en"}' >/dev/null
    curl -fs "$URL/Startup/User" >/dev/null
    post /Startup/User '{"Name":"test","Password":"test"}' >/dev/null
    post /Startup/RemoteAccess '{"EnableRemoteAccess":true,"EnableAutomaticPortMapping":false}' >/dev/null
    post /Startup/Complete '' >/dev/null
    token=$(post /Users/AuthenticateByName '{"Username":"test","Pw":"test"}' |
            python3 -c 'import sys, json; print(json.load(sys.stdin)["AccessToken"])')
    post "/Library/VirtualFolders?name=Music&collectionType=music&paths=%2Fmedia%2Fmusic&refreshLibrary=true" \
        '{"LibraryOptions":{}}' "$token" >/dev/null
    printf 'scanning the library'
    i=0
    until curl -fs "$URL/Items?Recursive=true&IncludeItemTypes=Audio" \
              -H "Authorization: $AUTH, Token=\"$token\"" | grep -q '"TotalRecordCount":3'; do
        i=$((i + 1))
        [ $i -gt 60 ] && { echo " - not finished, check the web UI" >&2; return; }
        printf .
        sleep 1
    done
    echo " done"
}

case "$1" in
start)
    mkdir -p "$DIR/config" "$DIR/cache" "$DIR/media/music"
    [ -n "$(ls "$DIR/media/music")" ] || make_media
    docker rm -f $NAME >/dev/null 2>&1 || true
    docker run -d --name $NAME --user "$ME" -p "$BIND:8096:8096" \
        -v "$DIR/config:/config" -v "$DIR/cache:/cache" -v "$DIR/media:/media:ro" \
        "$IMAGE" >/dev/null
    wait_up
    setup
    echo "Jellyfin: $URL (bound to $BIND), user test, password test"
    ;;
stop)
    docker rm -f $NAME >/dev/null 2>&1 || true
    echo "stopped"
    ;;
reset)
    docker rm -f $NAME >/dev/null 2>&1 || true
    rm -rf "$DIR"
    echo "stopped and deleted $DIR"
    ;;
*)
    echo "usage: $0 start|stop|reset" >&2
    exit 1
    ;;
esac
