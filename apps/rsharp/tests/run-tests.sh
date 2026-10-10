#!/bin/sh
# Host tests for rSharp's interpreter: builds it with the host compiler
# (with AddressSanitizer and UBSan) and runs tests/cases.txt.
#   apps/rsharp/tests/run-tests.sh [filter]
set -e
cd "$(dirname "$0")/.."
mkdir -p build
# The engine as on Symbian: gnu++98 (the harness itself is C++17).
FLAGS="-O1 -g -fno-exceptions -fno-rtti -Wall -Wno-unused-parameter -fsanitize=address,undefined -fno-sanitize-recover=undefined"
for f in engine/*.cpp; do
    ${CXX:-g++} -std=gnu++98 $FLAGS -c "$f" -o "build/$(basename "$f" .cpp).o"
done
${CXX:-g++} -std=c++17 $FLAGS tests/rs_test.cpp build/rs_*.o -o build/rs_test -lm
exec build/rs_test tests/cases.txt "$@"
