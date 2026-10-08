#!/usr/bin/env python3
"""Build an EKA2L1 emulator device from a Nokia BB5 firmware (core + variant
flash images), without EKA2L1's firmware installer.

  bb5-device.py --core <.c00/.fpsx> [--variant <.v16/.fpsx>] --data <EKA2L1 data dir>
                --firmcode RA-6 --model "E90 Communicator" [--platver epoc93fp1]

Writes what EKA2L1's installer would:
  <data>/roms/<firmcode>/SYM.ROM   the core ROM image
  <data>/drives/z/<firmcode>/...   the Z: drive: files of the ROM, ROFS1 and
                                   the variant (ROFS2), lower-cased
  <data>/devices.yml               a device entry (isolated drives)

Written for the Nokia E90 (RA-6), whose 2007 images EKA2L1 cannot install
(see env/tools/bb5-fix.py for the block layout):
- the core ROM is a raw deflate stream (inflated here);
- the variant is a "ROFx" extension image: its directory lists the whole
  merged Z: drive, and its file addresses are relative to the start of
  ROFS1, so entries for ROFS1's files point into ROFS1's data. They are
  resolved against both images here.
Standard library only.
"""
import argparse, os, struct, sys, zlib

CODE, CERT = 0x54, 0x5D
SIZE_AT = {0x17: 7, 0x27: 38, 0x28: 38}


# ---------------------------------------------------------------- BB5 images

def parse_blocks(d, padded):
    pos = 5 + struct.unpack('>I', d[1:5])[0]
    region, out = None, []
    while pos < len(d):
        ctype, btype, hs = d[pos], d[pos + 2], d[pos + 3]
        if btype not in SIZE_AT:
            return None
        h = d[pos + 3:pos + 4 + hs]
        size, addr = struct.unpack('>II', h[SIZE_AT[btype]:SIZE_AT[btype] + 8])
        data = pos + 4 + hs + 1
        if data + size > len(d):
            return None
        if ctype == CERT:
            region = h[21:33].split(b'\0')[0].decode('latin1')
        elif ctype == CODE:
            out.append((region, addr, d[data:data + size]))
        pos = data + (size + 511) // 512 * 512 if padded else data + size
    return out


def regions(path):
    """{region name: (flash address, bytes)} of a BB5 image (padded or not)."""
    d = open(path, 'rb').read()
    if d[0] not in (0xB0, 0xB1, 0xB2):
        sys.exit('%s: not a BB5 image' % path)
    blocks = parse_blocks(d, False) or parse_blocks(d, True)
    if not blocks:
        sys.exit('%s: cannot parse its blocks' % path)
    out = {}
    for name, addr, data in blocks:
        out.setdefault(name, []).append((addr, data))
    return {k: (v[0][0], b''.join(x for a, x in v)) for k, v in out.items()}


def core_rom(core):
    """The plain ROM image from the SOS*CORE region."""
    try:
        z = zlib.decompressobj(-15)
        rom = z.decompress(core)
        if z.eof and len(rom) > len(core):
            return rom
    except zlib.error:
        pass
    return core[0xC00:]          # uncompressed: skip the bootstrap, as EKA2L1 does


# ---------------------------------------------------------------- ROM (XIP)

def rom_files(rom):
    """[(path, bytes)] from the ROM file system (EKA2 TRomHeader)."""
    base, size, rootlist = struct.unpack('<III', rom[0x8C:0x98])
    if base != 0x80000000 or size > len(rom) * 2:
        sys.exit('ROM header not recognised (base 0x%x)' % base)

    def at(lin, n):
        o = lin - base
        return rom[o:o + n]

    ndirs = struct.unpack('<i', at(rootlist, 4))[0]
    hw, root = struct.unpack('<II', at(rootlist + 4, 8))     # first variant
    out = []

    def walk(dir_lin, path, depth=0):
        if depth > 32:
            return
        dsize = struct.unpack('<i', at(dir_lin, 4))[0]
        p, end = dir_lin + 4, dir_lin + 4 + dsize
        while p < end:
            esize, addr, att, nlen = struct.unpack('<IIBB', at(p, 10))
            name = at(p + 10, 2 * nlen).decode('utf-16le')
            if att & 0x10:
                walk(addr, path + name + '/', depth + 1)
            else:
                out.append((path + name, at(addr, esize)))
            p += (10 + 2 * nlen + 3) & ~3
    walk(root, '')
    print('ROM: %d root dir(s), %d files' % (ndirs, len(out)))
    return out


# ---------------------------------------------------------------- ROFS

HDR = '<4sBBHIIII'


def rofs_files(images):
    """[(path, bytes)] from ROFS images given as [(address base, bytes)],
    the first being the one whose directory tree is walked."""
    top_base, top = images[0]
    magic, hsize, _, ver, dir_off, dir_size, _, _ = struct.unpack(HDR, top[:24])
    file_offset = dir_off - hsize          # address of top's first byte
    spaces = [(file_offset if i == 0 else b - top_base + file_offset, img)
              for i, (b, img) in enumerate(images)]

    def read(addr, n):
        for start, img in spaces:
            if start <= addr and addr + n <= start + len(img):
                return img[addr - start:addr - start + n]
        return None

    modern = ver >= 0x200
    out, missing = [], 0

    def entry(p):
        esz = struct.unpack('<H', read(p, 2))[0]
        e = read(p, esz)
        o = 18 if modern else 2
        nameoff, att = e[o], e[o + 1]
        fsize, faddr = struct.unpack('<II', e[o + 2:o + 10])
        nlen = e[o + 11]
        name = e[nameoff:nameoff + 2 * nlen].decode('utf-16le')
        step = esz if modern else (esz + 3) & ~3
        return step, name, att, fsize, faddr

    def walk(d, path, depth=0):
        nonlocal missing
        if depth > 32:
            return
        ssize, _, _, fb_addr, fb_size = struct.unpack('<HBBII', read(d, 12))
        subs, p = [], d + 12
        while p - d < ssize:
            step, name, att, fsize, faddr = entry(p)
            subs.append((name, faddr))
            p += step
        if fb_addr:
            p = fb_addr
            while p - fb_addr < fb_size:
                step, name, att, fsize, faddr = entry(p)
                data = read(faddr, fsize) if fsize else b''
                if data is None:
                    missing += 1
                else:
                    out.append((path + name, data))
                p += step
        for name, faddr in subs:
            walk(faddr, path + name + '/', depth + 1)
    walk(dir_off, '')
    print('%s: %d files%s' % (magic.decode(), len(out),
                               ', %d unreadable' % missing if missing else ''))
    return out


# ---------------------------------------------------------------- output

def write_files(files, zdir):
    for path, data in files:
        dst = os.path.join(zdir, *path.lower().split('/'))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, 'wb') as f:
            f.write(data)


def add_device(yml, firmcode, model, platver):
    text = open(yml).read() if os.path.exists(yml) else ''
    if any(line.startswith(firmcode + ':') for line in text.splitlines()):
        print('%s already in devices.yml' % firmcode)
        return
    if text and not text.endswith('\n'):
        text += '\n'
    text += ('%s:\n  platver: %s\n  manufacturer: Nokia\n  firmcode: %s\n'
             '  model: %s\n  isolated-drives: true\n' % (firmcode, platver, firmcode, model))
    open(yml, 'w').write(text)
    print('added %s (%s) to devices.yml' % (firmcode, model))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--core', required=True)
    ap.add_argument('--variant')
    ap.add_argument('--data', required=True)
    ap.add_argument('--firmcode', required=True)
    ap.add_argument('--model', required=True)
    ap.add_argument('--platver', default='epoc93fp1')
    a = ap.parse_args()

    core = regions(a.core)
    if 'SOS*CORE' not in core or 'SOS+ROFS' not in core:
        sys.exit('%s: no SOS*CORE / SOS+ROFS regions' % a.core)
    rom = core_rom(core['SOS*CORE'][1])
    low = a.firmcode.lower()
    zdir = os.path.join(a.data, 'drives', 'z', low)
    romdir = os.path.join(a.data, 'roms', low)
    if os.path.exists(zdir) or os.path.exists(romdir):
        sys.exit('%s or %s already exists; remove them to rebuild' % (zdir, romdir))

    files = rom_files(rom)
    rofs1 = core['SOS+ROFS']
    files += rofs_files([rofs1])
    if a.variant:
        var = regions(a.variant)
        name = next((k for k in var if k and k.startswith('SOS+ROF')), None)
        if not name:
            sys.exit('%s: no ROFS/ROFx region' % a.variant)
        files += rofs_files([var[name], rofs1])   # later entries win

    os.makedirs(romdir)
    with open(os.path.join(romdir, 'SYM.ROM'), 'wb') as f:
        f.write(rom)
    write_files(files, zdir)
    print('SYM.ROM %d bytes; Z: %d files -> %s' % (len(rom), len(files), zdir))
    add_device(os.path.join(a.data, 'devices.yml'), a.firmcode, a.model, a.platver)


if __name__ == '__main__':
    main()
