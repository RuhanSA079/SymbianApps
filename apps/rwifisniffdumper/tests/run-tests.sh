#!/bin/sh
# Host tests for the driver: builds it with the host compiler
# (with AddressSanitizer and UBSan) and runs tests/rtlu_test.cpp.
#   apps/rwifisniffdumper/tests/run-tests.sh
set -e
cd "$(dirname "$0")/.."
mkdir -p build/tests
# The driver as on Symbian: gnu++98 (the harness itself is C++17).
FLAGS="-O1 -g -fno-exceptions -fno-rtti -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined -fno-sanitize-recover=undefined"
for f in driver/*.cpp; do
    ${CXX:-g++} -std=gnu++98 $FLAGS -c "$f" -o "build/tests/$(basename "$f" .cpp).o"
done
${CXX:-g++} -std=c++17 $FLAGS tests/rtlu_test.cpp build/tests/rtlu*.o -o build/tests/rtlu_test
exec build/tests/rtlu_test
