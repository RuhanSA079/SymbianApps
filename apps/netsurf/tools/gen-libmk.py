#!/usr/bin/env python3
"""Generate apps/netsurf/build/sym/Makefile: NetSurf's C libraries for
Symbian^3 (GCC 14, P.I.P.S.), one static archive per library.

The libraries are not built through abld: abld names objects by basename
(NetSurf has many same-named files) and passes -I-, which breaks NetSurf's
"same directory" includes. The flags below are the ones abld uses for GCCE
urel C code (see sdk/.../epoc32/build/.../*.gcce), so the archives link into
an ordinary .mmp with STATICLIBRARY.

Input: apps/netsurf/build/host/compile.json (env/netsurf-hostgen.sh).
Run inside the container:  make -C apps/netsurf/build/sym -j8
"""
import json, os, shlex

HERE = os.path.dirname(os.path.abspath(__file__))
APP = os.path.dirname(HERE)
SRC = os.path.join(APP, 'src')
GEN = os.path.join(APP, 'gen')
OUT = os.path.join(APP, 'build', 'sym')

LIBS = ['netsurf', 'libpng', 'libjpeg', 'libcompat', 'libexpat', 'libwapcaplet', 'libparserutils', 'libhubbub', 'libdom', 'libcss',
        'libnsutils', 'libnsbmp', 'libnsgif', 'librosprite', 'libutf8proc',
        'libnspsl', 'libnslog', 'libsvgtiny', 'libnsfb']

# Host-only defines (glibc feature macros) that mean nothing to P.I.P.S.
DROP_D = {'_GNU_SOURCE', '_GNU_SOURCE=1', '_REENTRANT', '_BSD_SOURCE',
          '_DEFAULT_SOURCE', '_POSIX_C_SOURCE=200809L', '_POSIX_C_SOURCE=200112L'}

# libnsfb: only the core and the RAM surface; the Symbian surface lives in
# the app (its SDL/VNC/X/Wayland/linux surfaces need host libraries).
NSFB_SKIP = ('src/surface/sdl.c', 'src/surface/vnc.c', 'src/surface/x.c',
             'src/surface/wld.c', 'src/surface/linux.c', 'src/surface/able.c')

comp = json.load(open(os.path.join(APP, 'build', 'host', 'compile.json')))
# Built here only: expat (libdom's XML parser; the host used the system's)
# and our P.I.P.S. compat code. Their "sources" are relative to SRC/<name>,
# so they are listed with ../ paths.
comp['libexpat'] = {
    'src': ['../../expat/lib/' + f for f in ('xmlparse.c', 'xmltok.c', 'xmlrole.c',
                                             'xcs.c', 'random_arc4random_buf.c')],
    'extra_I': ['$(APPDIR)/symbian/compat', '$(APPDIR)/expat/lib'],
}
comp['libcompat'] = {
    'src': ['../../symbian/compat/ns_compat.c', '../../symbian/compat/ns_random.cpp'],
    'extra_I': ['$(APPDIR)/symbian/compat'],
}
comp['libdom'].setdefault('extra_I', []).append('$(APPDIR)/expat/lib')

# PNG and JPEG decoders (env/fetch-imagelibs.sh); zlib comes from P.I.P.S.
PNG_SRC = ['png.c', 'pngerror.c', 'pngget.c', 'pngmem.c', 'pngpread.c',
           'pngread.c', 'pngrio.c', 'pngrtran.c', 'pngrutil.c', 'pngset.c',
           'pngtrans.c', 'pngwio.c', 'pngwrite.c', 'pngwtran.c', 'pngwutil.c']
comp['libpng'] = {
    'src': ['../../libpng/' + f for f in PNG_SRC],
    'D': ['PNG_ARM_NEON_OPT=0'],
    'extra_I': ['$(APPDIR)/libpng'],
}
def jpeg_lib_sources():
    """LIBSOURCES from IJG's makefile.ansi, plus the no-backing-store
    memory manager."""
    text = open(os.path.join(APP, 'libjpeg', 'makefile.ansi')).read().replace('\\\n', ' ')
    for line in text.splitlines():
        if line.startswith('LIBSOURCES='):
            return line.split('=', 1)[1].split() + ['jmemnobs.c']
    raise SystemExit('LIBSOURCES not found in libjpeg/makefile.ansi')
comp['libjpeg'] = {
    'src': ['../../libjpeg/' + f for f in jpeg_lib_sources()],
    'extra_I': ['$(APPDIR)/libjpeg'],
}

# NetSurf itself (core + framebuffer frontend) and our Symbian frontend
# code, as ns_core.lib. Left out: curl (replaced by symbian/fetch_rsym.c)
# and Duktape JavaScript (javascript/none instead, for now).
NS_SKIP = ('content/fetchers/curl.c', 'content/handlers/javascript/duktape/',
           'build/Linux-framebuffer/duktape/')
NS_DROP_D = {'WITH_CURL', 'WITH_OPENSSL',
             'DUK_OPT_HAVE_CUSTOM_H', 'LIBICONV_PLUG', '_NETBSD_SOURCE',
             '_XOPEN_SOURCE=700'}
NS_PRIVATE = '/private/E5A1E030'       # the app's SID
core = comp['netsurf']
core['src'] = [x for x in core['src'] if not x.startswith(NS_SKIP)] + [
    'content/handlers/javascript/none/none.c',
    '../../symbian/fetch_rsym.c', '../../symbian/nsfb_symbian.c',
    '../../symbian/nsfb_glue.c', '../../symbian/ns_thread.cpp']
core['D'] = [d for d in core['D'] if d not in NS_DROP_D and
             not d.startswith(('NETSURF_FB_RESPATH=', 'NETSURF_FB_FONTPATH=',
                               'NETSURF_LOG_LEVEL='))] + [
    'WITH_RSYM_FETCH', 'NSFB_SYMBIAN_UCS4_KEY=0x100000',
    'NETSURF_FB_RESPATH="%s/res"' % NS_PRIVATE,
    'NETSURF_FB_FONTPATH="%s/res"' % NS_PRIVATE,
    'NETSURF_LOG_LEVEL=INFO']
core['extra_I'] = ['$(APPDIR)/symbian', '$(APPDIR)/../common/net',
                   '$(APPDIR)/libpng', '$(APPDIR)/libjpeg']
# last, so NetSurf's own headers win: libnsfb's internal headers for the
# surface (nsfb_symbian.c)
core['late_I'] = ['$(NS)/libnsfb/src']

def src_path(lib, rel):
    """A source as the Makefile sees it (relative to OUT)."""
    for base in (SRC, GEN):
        p = os.path.normpath(os.path.join(base, lib, rel))
        if os.path.exists(p):
            return os.path.relpath(p, OUT)
    raise SystemExit('missing source %s/%s' % (lib, rel))

# The libraries' public headers exactly as their "make install" lays them
# out (dom/bindings/..., flat utf8proc.h), from the host build.
PUBINC = ['$(APPDIR)/build/host/tree/inst-framebuffer/include']

def gen_dirs(lib):
    """Directories of generated files (e.g. a .inc next to its .c)."""
    out = []
    for dp, dns, fns in sorted(os.walk(os.path.join(GEN, lib))):
        if fns:
            out.append('$(GEN)/' + os.path.relpath(dp, GEN))
    return out

mk = ["""# GENERATED by apps/netsurf/tools/gen-libmk.py, do not edit by hand.
NS      := %s
GEN     := %s
APPDIR  := %s
EPOC    := /opt/symbian/sdk/symbian3/epoc32
GCCE_BIN ?= /opt/symbian/gcc-14.2/bin
CC      := $(GCCE_BIN)/arm-none-symbianelf-gcc
CXX     := $(GCCE_BIN)/arm-none-symbianelf-g++
AR      := $(GCCE_BIN)/arm-none-symbianelf-ar
# As abld's GCCE urel rules, plus C99/GNU and quieter warnings.
ARMFLAGS := -O2 -fno-unit-at-a-time -march=armv5t -mapcs -mthumb-interwork \\
  -mthumb -msoft-float -nostdinc -pipe -Wno-unknown-pragmas \\
  -D__MARM_THUMB__ -D__MARM_INTERWORK__ -DNDEBUG -D_UNICODE -D__GCCE__ \\
  -D__SYMBIAN32__ -D__EPOC32__ -D__MARM__ -D__EABI__ -D__MARM_ARMV5__ \\
  -D__SUPPORT_CPP_EXCEPTIONS__ \\
  '-D__PRODUCT_INCLUDE__="$(EPOC)/include/variant/symbian_os.hrh"' \\
  -include $(EPOC)/include/gcce/gcce.h
SYMFLAGS := $(ARMFLAGS) -std=gnu99 -include $(APPDIR)/symbian/compat/ns_symbian.h
SYSINC  := -I$(APPDIR)/symbian/compat -I$(EPOC)/include/stdapis \\
  -I$(EPOC)/include -I$(EPOC)/include/variant \\
  -isystem $(shell $(CC) -print-file-name=include)
PUBINC  := %s
RELEASE := $(EPOC)/release/armv5/urel

ALL :=
""" % (os.path.relpath(SRC, OUT), os.path.relpath(GEN, OUT),
       os.path.relpath(APP, OUT), ' '.join('-I' + p for p in PUBINC))]

for lib in LIBS:
    c = comp[lib]
    srcs = [s for s in c['src'] if not (lib == 'libnsfb' and s.startswith(NSFB_SKIP))]
    if lib == 'libnsfb':
        srcs.append('src/surface/ram.c') if 'src/surface/ram.c' not in srcs else None
    defs = [d for d in c.get('D', []) if d not in DROP_D and d != 'NDEBUG']
    srcs = [x for x in srcs]
    incs = list(gen_dirs(lib)) + c.get('extra_I', [])
    for i in c.get('I', []):
        if i.startswith(('/usr/', 'inst-framebuffer')):
            continue
        i = i.rstrip('/')
        if i.startswith(lib + '/'):
            i = i[len(lib) + 1:]
        if i.startswith('build-'):
            incs.append('$(GEN)/%s/%s' % (lib, i))
        else:
            incs.append(os.path.normpath('$(NS)/%s/%s' % (lib, i)))
            incs.append(os.path.normpath('$(GEN)/%s/%s' % (lib, i)))
    incs += c.get('late_I', [])
    up = lib.upper().replace('LIB', 'L_', 1)
    objs = []
    mk.append('\n# ---- %s (%d sources)\n' % (lib, len(srcs)))
    mk.append('%s_FLAGS := %s %s\n' % (up, ' '.join(shlex.quote('-D' + d) for d in defs),
                                      ' '.join('-I' + i for i in incs)))
    for s in sorted(srcs):
        base, ext = os.path.splitext(s)
        o = 'obj/%s/%s.o' % (lib, base.replace('../', '').replace('/', '_'))
        objs.append(o)
        if ext == '.cpp':
            cmd = '$(CXX) $(ARMFLAGS) -fexceptions -x c++'
        else:
            cmd = '$(CC) $(SYMFLAGS)'
        # objects also depend on this Makefile, so changed flags rebuild them;
        # -MMD records header dependencies
        mk.append('%s: %s Makefile\n\t@mkdir -p $(@D)\n\t%s $(%s_FLAGS) $(PUBINC) $(SYSINC) -MMD -MP -c $< -o $@\n'
                  % (o, src_path(lib, s), cmd, up))
    arch = 'ns_%s.lib' % (lib[3:] if lib.startswith('lib') else 'core')
    mk.append('%s: %s\n\trm -f $@\n\t$(AR) cr $@ $^\n' % (arch, ' '.join(objs)))
    mk.append('ALL += %s\n' % arch)

mk.append("""
all: $(ALL)
install: $(ALL)
\tcp $(ALL) $(RELEASE)/
.PHONY: all install
.DEFAULT_GOAL := install
-include $(shell find obj -name '*.d' 2>/dev/null)
""")
os.makedirs(OUT, exist_ok=True)
# Only rewrite the Makefile when it changes: every object depends on it.
path = os.path.join(OUT, 'Makefile')
text = ''.join(mk)
old = open(path).read() if os.path.exists(path) else None
if text != old:
    open(path, 'w').write(text)
    print('wrote apps/netsurf/build/sym/Makefile (%d libraries)' % len(LIBS))
else:
    print('apps/netsurf/build/sym/Makefile is up to date')
