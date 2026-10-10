#!/bin/sh
# Builds build/host/rwifisniffdumper: the driver on Linux
# over libusb, for trying it with a real adapter (needs libusb-1.0 development
# files).
#   apps/rwifisniffdumper/host/build.sh && sudo apps/rwifisniffdumper/build/host/rwifisniffdumper -H 1-13 -w scan.pcap
set -e
cd "$(dirname "$0")/.."
mkdir -p build/host
FLAGS="-O2 -g -Wall -Wextra -Wno-unused-parameter"
for f in driver/*.cpp; do
    ${CXX:-g++} -std=gnu++98 $FLAGS -fno-exceptions -fno-rtti -c "$f" -o "build/host/$(basename "$f" .cpp).o"
done
${CXX:-g++} -std=c++17 $FLAGS host/rwifisniffdumper.cpp build/host/rtlu*.o -o build/host/rwifisniffdumper \
    $(pkg-config --cflags --libs libusb-1.0)
echo "built build/host/rwifisniffdumper"
