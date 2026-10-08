#!/bin/sh
# Add a Nokia E90 Communicator (RA-6, S60 3rd Edition FP1) to the EKA2L1
# emulator, built from Nokia's flash files. Runs on the HOST with the
# emulator closed (it rewrites devices.yml on exit). Needs curl, unzip,
# python3. Nokia's firmware is copyrighted: it stays in downloads/ and
# emu-data/ (both git-ignored).
#
#   env/fetch-firmware-e90.sh
#   ./emu --device RA-6 --install out/release/rssh-0.1.0-s60v3.sisx
#
# Source: firmware 400.34.93 from the "Nokia phone firmwares" collection on
# archive.org (one 24 GB zip; only the E90 entry is downloaded).
#
# EKA2L1's own firmware installer cannot use these 2007 images (unpadded
# flash blocks, a deflate-compressed core ROM, and a ROFx variant image
# whose directory points into ROFS1), so env/tools/bb5-device.py builds the
# device the way the installer would: roms/ra-6/SYM.ROM, the Z: drive in
# drives/z/ra-6, and a devices.yml entry with its own (isolated) C: and E:
# drives, so S60 3rd packages (with 2009 P.I.P.S. libraries) stay off the
# E7's E: drive.
set -e
VER=400.34.93
SHA256=19d511d689855546bc35c70fd0db8ab31afa6bb9c500151805f94ac4dfe64ee3
URL="https://archive.org/download/nokia-phone-firmwares/nokia_firmwares.zip/nokia_ra-6_v$VER.zip"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DL="$ROOT/downloads/firmware-e90"
DATA="$ROOT/emu-data/EKA2L1/data"
if pgrep -f '[t]ools/EKA2L1.AppImage' >/dev/null; then
    echo "close the emulator first (it rewrites devices.yml on exit)" >&2; exit 1
fi
[ -d "$DATA" ] || { echo "run ./emu once first to create emu-data/" >&2; exit 1; }
if [ -d "$DATA/drives/z/ra-6" ]; then
    echo "E90 (RA-6) is already installed in emu-data"; exit 0
fi

mkdir -p "$DL"
ZIP="$DL/nokia_ra-6_v$VER.zip"
[ -s "$ZIP" ] || curl -fL -o "$ZIP" "$URL"
echo "$SHA256  $ZIP" | sha256sum -c -

TMP=$(mktemp -d "$DL/.extract.XXXX")
trap 'rm -rf "$TMP"' EXIT
unzip -q "$ZIP" "ra-6_v$VER/*" -d "$TMP"
python3 -I "$ROOT/env/tools/bb5-device.py" \
    --core "$TMP/ra-6_v$VER/ra6_${VER}_prd.c00" \
    --variant "$TMP/ra-6_v$VER/ra6_${VER}_prd.v16" \
    --data "$DATA" --firmcode RA-6 --model "E90 Communicator" --platver epoc93fp1
# EKA2L1 crashes on the E90's window-server config (a hardware state with
# no alternate screen mode); see env/tools/wsini-fix.py.
python3 -I "$ROOT/env/tools/wsini-fix.py" "$DATA/drives/z/ra-6/system/data/wsini.ini"
echo "==> E90 ready: ./emu --device RA-6"
