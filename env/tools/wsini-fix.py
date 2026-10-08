#!/usr/bin/env python3
"""Make a phone's Z:\\system\\data\\wsini.ini safe for the EKA2L1 emulator.

  wsini-fix.py <wsini.ini>

EKA2L1's window server (window_server::parse_wsini) reads each hardware
state's S60_HWSTATE_SCREENMODE<n> and S60_HWSTATE_ALT_SCREENMODE<n> and
dereferences both when either exists, so a state without an alternate mode
crashes the emulator (null pointer in ini_pair::get) as soon as an app
starts. The Nokia E90's inner display (state 0) has no alternate mode; this
adds S60_HWSTATE_ALT_SCREENMODE<n> = the normal mode for any such state,
which is what the missing line means. The file's encoding (UTF-16 or 8-bit) and line endings are kept. Standard
library only.
"""
import re, sys

path = sys.argv[1]
raw = open(path, 'rb').read()
utf16 = raw[:2] in (b'\xff\xfe', b'\xfe\xff')
text = raw.decode('utf-16') if utf16 else raw.decode('latin1')
nl = '\r\n' if '\r\n' in text else '\n'
lines = text.split(nl)

normal = {}
alternate = set()
for i, line in enumerate(lines):
    m = re.match(r'\s*S60_HWSTATE_SCREENMODE(\d+)\s+(\d+)', line)
    if m:
        normal[m.group(1)] = (i, m.group(2))
    m = re.match(r'\s*S60_HWSTATE_ALT_SCREENMODE(\d+)\s', line)
    if m:
        alternate.add(m.group(1))

added = 0
for state, (i, mode) in sorted(normal.items(), key=lambda kv: -kv[1][0]):
    if state not in alternate:
        lines.insert(i + 1, 'S60_HWSTATE_ALT_SCREENMODE%s %s' % (state, mode))
        added += 1
if added:
    out = nl.join(lines)
    data = out.encode('utf-16') if utf16 else out.encode('latin1')   # utf-16 adds the BOM
    open(path, 'wb').write(data)
print('%s: added %d alternate screen mode line(s)' % (path, added))
