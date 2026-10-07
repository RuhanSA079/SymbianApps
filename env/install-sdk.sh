#!/bin/sh
# Runs INSIDE the container (via ./sym). Unpacks + patches the Symbian^3 SDK
# into /opt/symbian/sdk/symbian3 (= ./sdk/symbian3 on the host).
set -e
ZIP=/work/downloads/Symbian_3_SDK_v1_0_en.zip
DEST=/opt/symbian/sdk/symbian3
[ -f "$ZIP" ] || { echo "missing $ZIP - run env/fetch.sh first"; exit 1; }
[ -d "$DEST/epoc32" ] && { echo "$DEST already installed; delete ./sdk/symbian3 to reinstall"; exit 1; }
# GnuPoc's installer must run from its own dir and writes scratch files there.
TMP=$(mktemp -d /opt/symbian/sdk/.install.XXXX)
cp -r /opt/symbian/src/gnupoc-package/sdks "$TMP/"
cd "$TMP/sdks"
./install_gnupoc_symbian3 "$ZIP" "$DEST"
cd / && rm -rf "$TMP"
# Perl >= 5.22 compatibility for the SDK build scripts.
cd "$DEST/epoc32/tools" && grep -rlE 'defined *\(? *[%@]' --include=*.pm --include=*.pl . | xargs -r perl /work/env/fix-perl.pl
# Build-rule overrides for modern GCC (no effect with the stock GCC 3.4.3).
EPOCROOT="$DEST/" sh /work/env/patch-sdk-gcce.sh
echo "Symbian^3 SDK installed in $DEST"
