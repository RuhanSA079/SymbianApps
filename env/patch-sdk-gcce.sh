#!/bin/sh
# Teach the SDK's GCCE build rules about modern GCC (>= 4), without changing
# anything for the stock GCC 3.4.3. Appends an override block to gcce.mk;
# idempotent. Runs INSIDE the container (called by install-sdk.sh).
set -e
MK="${EPOCROOT:?}epoc32/tools/compilation_config/gcce.mk"
MARK="# --- SymbianApps: modern GCC overrides ---"
grep -qF "$MARK" "$MK" && { echo "gcce.mk already patched"; exit 0; }
cat >> "$MK" <<EOF

$MARK
ifneq "\$(GCC_MAJOR)" "3"
# -fno-unit-at-a-time was removed in GCC 4.4; -mapcs (APCS frames) is obsolete.
REL_OPTIMISATION=-O2
AAPCS_OPTION=-mthumb-interwork
# Symbian headers predate C++11 (dynamic exception specs, 'register', ...).
# The two -Wno-* silence noise from the SDK's own (C++) headers.
CPP_LANG_OPTION=-x c++ -std=gnu++98 -Wno-ctor-dtor-privacy -Wno-invalid-offsetof -Wno-return-local-addr
# Nokia's RVCT-built libraries are tagged "variable-size enums", but the
# Symbian ABI uses int-sized enums (RVCT --enum_is_int), matching GCC.
EXTRA_LD_OPTION=--no-enum-size-warning
endif
EOF
echo "patched $MK"
