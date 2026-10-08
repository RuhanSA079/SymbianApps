#!/usr/bin/env python3
"""Pick the results out of env/netsurf-hostgen.sh's Linux build.

  hostgen-extract.py <src> <hostbuild> <gen>

- <hostbuild>/compile.json: per component, the C sources it compiled and
  the -D / -I flags (from the Q= make log), as input for the Symbian .mmp files.
- <gen>/<component>/<path>: every source-like file the build created that
  is not in <src> (gperf/perl/python output, nsgenbind bindings, images,
  font, Messages), at the same relative path.
"""
import json, os, re, shlex, shutil, sys

src, hb, gen = sys.argv[1:4]
tree = os.path.join(hb, 'tree')

comps = {}
cur = None
for line in open(os.path.join(hb, 'build.log'), errors='replace'):
    m = re.search(r"Entering directory '/hb/tree/([^/']+)", line)
    if m:
        cur = m.group(1)
        continue
    if not cur or ' -c ' not in line:
        continue
    try:
        t = shlex.split(line)
    except ValueError:
        continue
    if not t or not re.search(r'(^|[-/])(gcc|cc)$', t[0]):
        continue
    cs = [x for x in t if x.endswith('.c')]
    if not cs:
        continue
    d = comps.setdefault(cur, {'src': set(), 'D': set(), 'I': set()})
    d['src'].add(cs[-1])
    for x in t:
        if x.startswith('-D'):
            d['D'].add(x[2:])
        elif x.startswith('-I'):
            d['I'].add(x[2:].replace('/hb/tree/', ''))
json.dump({k: {a: sorted(b) for a, b in v.items()} for k, v in comps.items()},
          open(os.path.join(hb, 'compile.json'), 'w'), indent=1)

KEEP = re.compile(r'(\.(c|h|inc)|/Messages)$')
SKIP = re.compile(r'^(inst-framebuffer/|[^/]+/build-[^/]*-binary/|netsurf/build/[^/]+/tools/)')
n = 0
for dp, dns, fns in os.walk(tree):
    for f in fns:
        rel = os.path.relpath(os.path.join(dp, f), tree)
        if SKIP.match(rel) or not KEEP.search(rel) or rel.endswith('.tmp'):
            continue
        if os.path.lexists(os.path.join(src, rel)):
            continue
        out = os.path.join(gen, rel)
        os.makedirs(os.path.dirname(out), exist_ok=True)
        shutil.copy2(os.path.join(dp, f), out)
        n += 1
print('compile.json: %d components; %d generated files' % (len(comps), n))
