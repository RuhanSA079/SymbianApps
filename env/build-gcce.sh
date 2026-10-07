#!/bin/sh
# Build a modern GCC cross compiler for Symbian (arm-none-symbianelf) from
# upstream sources: binutils 2.29.1 (the last release with a working
# symbianelf target) + GCC with C/C++, libgcc and the freestanding libsupc++.
# No libc is built: Symbian code links against the SDK's .dso import libraries.
#
#   env/build-gcce.sh <prefix> [workdir]     (inside the ubuntu:16.04 env image)
set -e
PREFIX=${1:?usage: build-gcce.sh <prefix> [workdir]}
WORK=${2:-/tmp/gcce-build}
TARGET=arm-none-symbianelf
BINUTILS=binutils-2.29.1
BINUTILS_SHA256=e7010a46969f9d3e53b650a518663f98a5dde3c3ae21b7d71e5e6803bc36b577
GCC_VER=14.2.0
GCC_SHA256=a7b39bc69cbf9e25826c5a60ab26477001f7c08d85cec04bc0e29cabed6f3cc9
JOBS=${JOBS:-$(nproc)}
# ftp.gnu.org is often unreachable; kernel.org mirrors it. Checksums below were
# taken from tarballs verified against the GNU keyring signatures.
GNU=${GNU_MIRROR:-https://mirrors.kernel.org/gnu}

mkdir -p "$WORK" "$PREFIX"
cd "$WORK"

fetch() { # url sha256
    f=$(basename "$1")
    [ -f "$f" ] || { [ -f "/work/downloads/gcce/$f" ] && cp "/work/downloads/gcce/$f" .; } || wget -q "$1"
    echo "$2  $f" | sha256sum -c -
}
fetch $GNU/binutils/$BINUTILS.tar.xz $BINUTILS_SHA256
fetch $GNU/gcc/gcc-$GCC_VER/gcc-$GCC_VER.tar.xz $GCC_SHA256
[ -d $BINUTILS ] || tar -xf $BINUTILS.tar.xz
if [ ! -d gcc-$GCC_VER ]; then
    tar -xf gcc-$GCC_VER.tar.xz
    # GMP/MPFR/MPC/ISL, built in-tree (checksummed by the script itself).
    (cd gcc-$GCC_VER && ./contrib/download_prerequisites)
fi
# GCC's symbianelf target never got the stdint type macros (__INTPTR_TYPE__,
# __UINTPTR_TYPE__, ...) that every other bare-metal ARM target has; libgcc and
# libsupc++ need them. Add newlib-stdint.h like arm-eabi (macros only: GCC's own
# stdint.h is not installed, so the SDK's Open C <stdint.h> stays in charge).
grep -q 'arm/symbian.h newlib-stdint.h' gcc-$GCC_VER/gcc/config.gcc || \
    sed -i 's#tm_file="${tm_file} arm/symbian.h"#tm_file="${tm_file} arm/symbian.h newlib-stdint.h"#' \
        gcc-$GCC_VER/gcc/config.gcc
grep -q 'arm/symbian.h newlib-stdint.h' gcc-$GCC_VER/gcc/config.gcc

# ---- binutils ----
if [ ! -x "$PREFIX/bin/$TARGET-ld" ]; then
    rm -rf build-binutils && mkdir build-binutils && cd build-binutils
    ../$BINUTILS/configure --target=$TARGET --prefix="$PREFIX" \
        --disable-nls --disable-shared --disable-werror --disable-gdb \
        MAKEINFO=true
    make -j"$JOBS" MAKEINFO=true
    make install-strip MAKEINFO=true
    cd ..
fi
export PATH="$PREFIX/bin:$PATH"

# Minimal target headers so libgcc/libsupc++ can build without a libc. sys-include
# is only on the compiler's default search path; SDK builds use -nostdinc.
SYSINC="$PREFIX/$TARGET/sys-include"
mkdir -p "$SYSINC"
[ -f "$SYSINC/stdio.h" ] || cat > "$SYSINC/stdio.h" <<'EOF'
/* Just enough for GCC's freestanding runtime configure checks. */
#ifndef _GCCE_SYS_STDIO_H
#define _GCCE_SYS_STDIO_H
#define EOF (-1)
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif
EOF

# libsupc++ also wants <stdint.h>; use GCC's own (built from the type macros above).
cp "$WORK/gcc-$GCC_VER/gcc/ginclude/stdint-gcc.h" "$SYSINC/stdint.h"

# ---- gcc ----
# gcov (coverage) is off: libgcov needs __INTPTR_TYPE__, which the symbianelf
# target does not define, and coverage is of no use on the phone anyway.
rm -rf build-gcc && mkdir build-gcc && cd build-gcc
../gcc-$GCC_VER/configure --target=$TARGET --prefix="$PREFIX" \
    --enable-languages=c,c++ --without-headers --with-newlib \
    --disable-hosted-libstdcxx --disable-libstdcxx-pch \
    --disable-shared --disable-threads --disable-nls --disable-multilib \
    --disable-libssp --disable-libquadmath --disable-libgomp --disable-libatomic \
    --disable-lto --disable-gcov --with-dwarf2 \
    --with-arch=armv5t --with-float=soft \
    --with-pkgversion="SymbianApps GCCE" \
    MAKEINFO=true
make -j"$JOBS" all-gcc all-target-libgcc MAKEINFO=true
make install-strip-gcc install-strip-target-libgcc MAKEINFO=true
# libsupc++ (C++ EH/RTTI runtime) from the freestanding libstdc++ build.
make -j"$JOBS" all-target-libstdc++-v3 MAKEINFO=true
make install-strip-target-libstdc++-v3 MAKEINFO=true
cd ..
echo "==> $($PREFIX/bin/$TARGET-gcc --version | head -1) in $PREFIX"
