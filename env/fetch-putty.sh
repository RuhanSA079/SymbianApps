#!/bin/sh
# Download the last s2putty release (PuTTY for Symbian OS 1.5.2, S60 3rd ed, 2010),
# verify its PGP signature and unpack it into downloads/putty/. Runs on the HOST.
#   env/fetch-putty.sh
#   ./emu --device RM-626 --install downloads/putty/putty_s60v3_1.5.2.sisx
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VER=1.5.2
ZIP=putty_s60v3_$VER.zip
BASE=https://downloads.sourceforge.net/project/s2putty/s2putty/$VER
# Petteri Kangaslampi <pekangas@s2.org>, s2putty maintainer (public key in env/).
FPR=7F07B920D60A95ECA84299F0354E937CE393AD7C

OUT="$ROOT/downloads/putty"
mkdir -p "$OUT"
cd "$OUT"
for f in "$ZIP" "$ZIP.asc"; do
    [ -s "$f" ] || wget -q --user-agent=Wget -O "$f" "$BASE/$f"
done

# Verify with a throwaway keyring holding only the pinned key.
G=$(mktemp -d)
trap 'rm -rf "$G"' EXIT
gpg -q --homedir "$G" --import "$ROOT/env/s2putty-signing-key.asc" 2>/dev/null
gpg --homedir "$G" --status-fd 1 --verify "$ZIP.asc" "$ZIP" 2>/dev/null \
    | grep -q "VALIDSIG $FPR" || { echo "PGP signature check FAILED for $ZIP" >&2; exit 1; }
echo "signature OK (Petteri Kangaslampi, $FPR)"

unzip -oq "$ZIP"
echo "==> downloads/putty/putty_s60v3_$VER.sisx"
