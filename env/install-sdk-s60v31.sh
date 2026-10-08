#!/bin/sh
# Runs INSIDE the container (via ./sym). Unpacks + patches the S60 3rd Edition
# FP1 SDK (Symbian OS 9.2) with Open C into /opt/symbian/sdk/s60v31
# (= ./sdk/s60v31 on the host). Run env/fetch-s60v31.sh on the host first.
# Build against it with SYM_SDK=s60v31 (see ./sym, env/build.sh).
set -e
DL=/work/downloads/s60v31
ZIP=$DL/s60v3.1_SDK.zip
OPENC=$DL/OpenC_3_0_SDK_plugin.zip
DEST=/opt/symbian/sdk/s60v31
for f in "$ZIP" "$OPENC"; do
    [ -f "$f" ] || { echo "missing $f - run env/fetch-s60v31.sh first"; exit 1; }
done
[ -d "$DEST/epoc32" ] && { echo "$DEST already installed; delete ./sdk/s60v31 to reinstall"; exit 1; }

# GnuPoc's installer must run from its own dir and writes scratch files there.
TMP=$(mktemp -d /opt/symbian/sdk/.install.XXXX)
cp -r /opt/symbian/src/gnupoc-package/sdks "$TMP/"
cd "$TMP/sdks"
# archive.org's zip is a 2014 repackaging: a folder (with spaces in its name)
# holding an InstallShield set whose SelfRegFiles2 group is Nokia's original
# installer. GnuPoc's installer expects Nokia's files at the top of the zip,
# so unwrap both layers right after its unzip step.
sed -i 's#^unzip -qn $SRC -d _e$#unzip -qn $SRC -d _e \&\& mv _e/*/* _e/ \&\& unshield -g SelfRegFiles2 x _e/data2.cab \&\& rm _e/data1.cab _e/data1.hdr _e/data2.cab \&\& mv SelfRegFiles2/* _e/ \&\& rmdir SelfRegFiles2#' install_gnupoc_s60_31
grep -q 'mv SelfRegFiles2/\* _e/' install_gnupoc_s60_31 || { echo "install_gnupoc_s60_31 changed; adjust the sed above" >&2; exit 1; }
./install_gnupoc_s60_31 "$ZIP" "$DEST"

# Open C (P.I.P.S.): an epoc32 tree; lower-case it like the SDK and merge.
mkdir openc
unzip -q "$OPENC" -d openc
./lowercase openc/epoc32
./fixinclude openc/epoc32/include
./mergedir openc/epoc32 "$DEST/epoc32"
# Symbian^3 has the Open C headers in include/stdapis; here they are in
# include/osextensions/stdapis. Link them so .mmp files work with both SDKs.
[ -e "$DEST/epoc32/include/stdapis" ] || ln -s osextensions/stdapis "$DEST/epoc32/include/stdapis"
cd / && rm -rf "$TMP"

# This Open C's stdbool.h never defines bool for Symbian targets (the
# Symbian^3 one does), so C99 code using bool fails to compile.
SB="$DEST/epoc32/include/stdapis/stdbool.h"
grep -q '^#define bool    _Bool' "$SB" || \
    sed -i 's|^#endif /\* !__cplusplus \*/|#define bool    _Bool\n\n#endif /* !__cplusplus */|' "$SB"
grep -q '^#define bool    _Bool' "$SB" || { echo "could not patch $SB" >&2; exit 1; }
# Perl >= 5.22 compatibility for the SDK build scripts.
cd "$DEST/epoc32/tools" && grep -rlE 'defined *\(? *[%@]' --include=*.pm --include=*.pl . | xargs -r perl /work/env/fix-perl.pl
# Build-rule overrides for modern GCC (no effect with the stock GCC 3.4.3).
EPOCROOT="$DEST/" sh /work/env/patch-sdk-gcce.sh
echo "S60 3rd FP1 SDK installed in $DEST"
