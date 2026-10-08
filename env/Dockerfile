# Symbian^3 (Nokia E7 / Anna / Belle) build environment.
#
# GnuPoc (native Linux replacements for the SDK's Windows tools) + CodeSourcery
# GCCE arm-none-symbianelf. Ubuntu 16.04 is used on purpose: GnuPoc was last
# touched in 2016 and its tools (make 3.81, OpenSSL 1.0 signsis, ...) build
# there unmodified.
#
# The SDK itself is NOT baked in; it is installed once into ./sdk on the host
# (see env/install-sdk.sh) and bind-mounted at /opt/symbian/sdk.
#
# Stages: base (OS packages) -> gcce (modern GCC, built from source by
# build-gcce.sh, ~30 min, cached) -> final image (GnuPoc + both compilers).

FROM ubuntu:16.04 AS base

ARG GNUPOC_COMMIT=d3ddaf58c734ad3ab34dc60a012f9d4768a9ed3e
ARG GCCE_URL=http://www.martin.st/symbian/gnu-csl-arm-2005Q1C-arm-none-symbianelf-i686-pc-linux-gnu.tar.bz2
ARG GCCE_SHA256=cd80c3068b69d2f7ebd9dc917cdd6353158d465623e8ec4ac244915143ce5ce0

ENV DEBIAN_FRONTEND=noninteractive
RUN dpkg --add-architecture i386 && apt-get update && apt-get install -y --no-install-recommends \
        build-essential g++ make patch perl python unzip zip bzip2 xz-utils \
        flex bison zlib1g-dev libssl-dev ca-certificates wget git file \
        libc6:i386 libstdc++6:i386 zlib1g:i386 m4 \
    && rm -rf /var/lib/apt/lists/*

# ---- modern GCC (arm-none-symbianelf); the default compiler, see ENV below ----
FROM base AS gcce
COPY build-gcce.sh /tmp/build-gcce.sh
RUN sh /tmp/build-gcce.sh /opt/symbian/gcc-14.2 /tmp/gcce-build && rm -rf /tmp/gcce-build

# ---- final image ----
FROM base

# python3: NetSurf's build steps (apps/netsurf/prebuild.sh). Added here, not
# in base, so the GCC stage stays cached.
RUN apt-get update && apt-get install -y --no-install-recommends python3 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /opt/symbian/src

# GnuPoc, pinned.
RUN git clone https://github.com/mstorsjo/gnupoc-package.git \
    && cd gnupoc-package && git checkout -q ${GNUPOC_COMMIT}

# Rebuild the bundled (patched, S60 3.2+-capable) unshield from source instead
# of trusting the prebuilt i386 binary.
RUN cd gnupoc-package/sdks/unshield \
    && tar -zxf unshield-svn-3953.tar.gz && cd unshield-svn \
    && ./configure --enable-shared=no >/dev/null && make >/dev/null \
    && cp src/unshield ../unshield

# CodeSourcery GCCE 2005Q1C (gcc 3.4.3) for arm-none-symbianelf.
RUN mkdir -p /opt/symbian/csl-gcc && cd /opt/symbian/csl-gcc \
    && wget -q -O /tmp/gcce.tar.bz2 "${GCCE_URL}" \
    && if [ -n "${GCCE_SHA256}" ]; then echo "${GCCE_SHA256}  /tmp/gcce.tar.bz2" | sha256sum -c -; fi \
    && tar -jxf /tmp/gcce.tar.bz2 && rm /tmp/gcce.tar.bz2

# Native EKA2 tools (elf2e32, rcomp, makesis, signsis, makekeys, mifconv, ...).
RUN cd gnupoc-package/tools && ./install_eka2_tools /opt/symbian/csl-gcc

# Wrapper scripts (abld, bldmake, ...) pointing at our toolchain.
RUN cd gnupoc-package/sdks && ./install_wrapper /opt/symbian/gnupoc \
    && sed -i 's#^EKA2TOOLS=.*#EKA2TOOLS=/opt/symbian/csl-gcc/bin#' /opt/symbian/gnupoc/gnupoc-common.sh \
    && echo '# GCCE_BIN=<dir> selects another compiler (it must win over EKA2TOOLS).' >> /opt/symbian/gnupoc/gnupoc-common.sh \
    && echo '[ -n "$GCCE_BIN" ] && export PATH="$GCCE_BIN:$PATH"' >> /opt/symbian/gnupoc/gnupoc-common.sh

COPY --from=gcce /opt/symbian/gcc-14.2 /opt/symbian/gcc-14.2

# GCC 14 is the default compiler; GCCE_BIN=/opt/symbian/csl-gcc/bin (e.g.
# ./sym with that variable set) selects the original GCCE 3.4.3 instead.
ENV EPOCROOT=/opt/symbian/sdk/symbian3/ \
    GCCE_BIN=/opt/symbian/gcc-14.2/bin \
    PATH=/opt/symbian/gnupoc:/opt/symbian/csl-gcc/bin:$PATH

WORKDIR /work
