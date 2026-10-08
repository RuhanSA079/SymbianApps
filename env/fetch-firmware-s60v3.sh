#!/bin/sh
# Add a Nokia E71 (RM-346, S60 3rd Edition FP1 = Symbian OS 9.2) to the EKA2L1
# emulator, to test S60 3rd builds (SYM_SDK=s60v31) - the E90 runs the same
# platform. Runs on the HOST with the emulator closed (it rewrites
# devices.yml on exit). Needs curl, 7z, sha256sum.
#
# The E71 gets its own C: and E: drives (isolated-drives): S60 3rd packages
# carry Nokia's 2009 P.I.P.S. libraries, which must not land on the E7's
# shared E: drive, where they would shadow the E7's newer ones.
#
#   env/fetch-firmware-s60v3.sh
#   ./emu --device RM-346 --install out/rssh_s60v31.sisx
#
# Why not the E90 itself: its only available firmware (RA-6 400.34.93, see
# env/fetch-firmware-e90.sh) is a 2007-era flash image that EKA2L1's firmware
# installer cannot unpack ("Untested ROFx variant", unreadable ROFS blocks).
# This is a ready-made EKA2L1 dump (ROM + extracted Z: drive) from the
# archive.org item images-1_202601; no digest is published, so the SHA-256
# was recorded from the first download. Nokia's firmware is copyrighted: it
# stays in downloads/ and emu-data/ (both git-ignored).
set -e
SHA256=355c333983cdbb7d8e0d814bdc519d79a778123978cea05661f2e82fd0c12c41
URL="https://archive.org/download/images-1_202601/Symbian%20OS%20rom.zip/Symbian%20OS%20rom%20dump%20%28for%20eka2l1%20emulator%29%2Fs60v3%2FE71%20%28S60v3%20FP1%29.7z"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DL="$ROOT/downloads/firmware-s60v3"
DATA="$ROOT/emu-data/EKA2L1/data"
ARCHIVE="$DL/E71 (S60v3 FP1).7z"

if pgrep -f '[t]ools/EKA2L1.AppImage' >/dev/null; then
    echo "close the emulator first (it rewrites devices.yml on exit)" >&2; exit 1
fi
[ -d "$DATA" ] || { echo "run ./emu once first to create emu-data/" >&2; exit 1; }
mkdir -p "$DL"
[ -s "$ARCHIVE" ] || curl -fL -o "$ARCHIVE" "$URL"
echo "$SHA256  $ARCHIVE" | sha256sum -c -

if [ -d "$DATA/drives/z/rm-346" ]; then
    echo "E71 (RM-346) files already in emu-data"
else
    TMP=$(mktemp -d "$DL/.extract.XXXX")
    7z x -bd -y -o"$TMP" "$ARCHIVE" >/dev/null
    mkdir -p "$DATA/drives/z" "$DATA/roms"
    mv "$TMP/E71 (S60v3)/data/drives/z/rm-346" "$DATA/drives/z/"
    mv "$TMP/E71 (S60v3)/data/roms/rm-346" "$DATA/roms/"
    rm -rf "$TMP"
fi
# The dump names it SYM.rom; EKA2L1 opens SYM.ROM (case matters on Linux),
# and without it silently falls back to the first device.
[ -f "$DATA/roms/rm-346/SYM.rom" ] && mv "$DATA/roms/rm-346/SYM.rom" "$DATA/roms/rm-346/SYM.ROM"

YML="$DATA/devices.yml"
if grep -q '^RM-346:' "$YML" 2>/dev/null; then
    echo "RM-346 already in devices.yml"
else
    # make sure the previous entry ends with a newline before appending
    [ -s "$YML" ] && [ "$(tail -c1 "$YML")" != "" ] && echo >> "$YML"
    cat >> "$YML" <<'YAML'
RM-346:
  platver: epoc93fp1
  manufacturer: Nokia
  firmcode: RM-346
  model: E71
  isolated-drives: true
YAML
    echo "added RM-346 (E71) to devices.yml"
fi
echo "==> E71 ready: ./emu --device RM-346"
