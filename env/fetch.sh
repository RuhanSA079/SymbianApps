#!/bin/sh
# Downloads the Symbian^3 SDK (not redistributable by us; mirrored on archive.org).
set -e
mkdir -p "$(dirname "$0")/../downloads"
cd "$(dirname "$0")/../downloads"
SUM="8b34b9bf6baf932d850229ba527d1067  Symbian_3_SDK_v1_0_en.zip"
if [ -f Symbian_3_SDK_v1_0_en.zip ] && echo "$SUM" | md5sum -c - >/dev/null 2>&1; then
    echo "Symbian_3_SDK_v1_0_en.zip: already downloaded (MD5 OK)"
    exit 0
fi
wget -c https://archive.org/download/symbian-3-sdk-v-1-0-en_202207/Symbian_3_SDK_v1_0_en.zip
echo "$SUM" | md5sum -c -
