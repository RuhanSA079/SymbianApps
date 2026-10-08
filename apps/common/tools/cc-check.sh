#!/bin/sh
# Compile-check the shared C code (mbedTLS + Symbian glue) with the flags
# abld uses for GCCE urel. Runs INSIDE the container, from apps/common:
#   ../../sym sh tools/cc-check.sh [files...]
. /opt/symbian/gnupoc/gnupoc-common.sh
E=${EPOCROOT}epoc32
OUT=build/obj
LOGS=build/cc-check
mkdir -p $OUT $LOGS
rm -f $LOGS/*.log
INC=""
for d in $(cat group/mbedtls-includes-all.txt); do INC="$INC -Imbedtls/$d"; done
SYSINC=""
for d in $(cat group/mbedtls-includes.txt); do SYSINC="$SYSINC -Imbedtls/$d"; done
COMMON="-O2 -march=armv5t -mthumb-interwork -pipe -nostdinc -Wall -Wno-unknown-pragmas
 -include $E/include/gcce/gcce.h
 -D__SYMBIAN32__ -D__GCCE__ -D__EPOC32__ -D__MARM__ -D__EABI__ -D__MARM_ARMV5__ -D__EXE__
 -D__SUPPORT_CPP_EXCEPTIONS__ -D__PRODUCT_INCLUDE__=\"$E/include/variant/symbian_os.hrh\"
 -DNDEBUG -D_UNICODE
 -Imbedtls-symbian -Inet $INC -I- $SYSINC -Imbedtls-symbian
 -I$E/include/stdapis -I$E/include -I$E/include/mw -I$E/include/platform
 -I$E/include/platform/mw"
CFLAGS="-x c $COMMON"
CXXFLAGS="-x c++ -std=gnu++98 -fexceptions -Wno-ctor-dtor-privacy -Wno-invalid-offsetof -Wno-return-local-addr $COMMON"
export CFLAGS CXXFLAGS OUT LOGS
FILES=${*:-$(sed 's#^#mbedtls/#' group/mbedtls-sources.txt) $(ls mbedtls-symbian/*.c mbedtls-symbian/*.cpp net/*.c net/*.cpp 2>/dev/null)}
printf '%s\n' $FILES | xargs -P "$(nproc)" -I{} sh -c '
  f={}; n=$(echo "$f" | tr / _)
  case "$f" in *.cpp) FL=$CXXFLAGS ;; *) FL=$CFLAGS ;; esac
  arm-none-symbianelf-gcc $FL -c "$f" -o $OUT/${n%.*}.o > $LOGS/$n.log 2>&1 || echo "FAIL $f" >> $LOGS/$n.log'
fails=$(grep -l '^FAIL ' $LOGS/*.log 2>/dev/null | wc -l)
total=$(printf '%s\n' $FILES | wc -l)
echo "compiled $((total - fails))/$total OK"
grep -h -E ' error: |#error' $LOGS/*.log | sed -E 's/^[^ ]*: //' | sort | uniq -c | sort -rn | head -${TOP:-25}
