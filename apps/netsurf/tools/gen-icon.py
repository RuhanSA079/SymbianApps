#!/usr/bin/env python3
"""Make the app icon from NetSurf's own logo (frontends/gtk/res/netsurf.xpm):
a square, smoothly downscaled 24-bit BMP plus an 8-bit soft mask, as
mifconv wants them (<name>.bmp + <name>_mask_soft.bmp, used with /c24,8).

  gen-icon.py <netsurf.xpm> <out-dir> [size]

Standard library only (runs inside the build container).
"""
import os, struct, sys

def read_xpm(path):
    text = open(path).read()
    strings = []
    i = 0
    while True:
        a = text.find('"', i)
        if a < 0:
            break
        b = text.find('"', a + 1)
        strings.append(text[a + 1:b])
        i = b + 1
    w, h, ncolors, cpp = (int(x) for x in strings[0].split()[:4])
    colors = {}
    for line in strings[1:1 + ncolors]:
        key, rest = line[:cpp], line[cpp:].split()
        value = rest[rest.index('c') + 1]
        if value.lower() == 'none':
            colors[key] = (0, 0, 0, 0)
        elif value.startswith('#') and len(value) == 7:
            colors[key] = (int(value[1:3], 16), int(value[3:5], 16), int(value[5:7], 16), 255)
        elif value.lower() == 'black':
            colors[key] = (0, 0, 0, 255)
        elif value.lower() == 'white':
            colors[key] = (255, 255, 255, 255)
        elif value.lower().startswith(('gray', 'grey')) and value[4:].isdigit():
            v = round(int(value[4:]) * 255 / 100)      # X11 grayN: N percent
            colors[key] = (v, v, v, 255)
        else:
            raise SystemExit('unsupported XPM colour %r' % value)
    rows = strings[1 + ncolors:1 + ncolors + h]
    return w, h, [[colors[r[x * cpp:(x + 1) * cpp]] for x in range(w)] for r in rows]

def to_square(w, h, px):
    """Pad with transparency to a centred square."""
    n = max(w, h)
    ox, oy = (n - w) // 2, (n - h) // 2
    out = [[(0, 0, 0, 0)] * n for _ in range(n)]
    for y in range(h):
        out[oy + y][ox:ox + w] = px[y]
    return n, out

def downscale(n, px, size):
    """Area-average resampling with premultiplied alpha."""
    out = []
    scale = n / size
    for oy in range(size):
        y0, y1 = oy * scale, (oy + 1) * scale
        row = []
        for ox in range(size):
            x0, x1 = ox * scale, (ox + 1) * scale
            acc = [0.0, 0.0, 0.0, 0.0]
            total = 0.0
            for sy in range(int(y0), min(n, int(y1) + 1)):
                wy = min(y1, sy + 1) - max(y0, sy)
                if wy <= 0:
                    continue
                for sx in range(int(x0), min(n, int(x1) + 1)):
                    wx = min(x1, sx + 1) - max(x0, sx)
                    if wx <= 0:
                        continue
                    r, g, b, a = px[sy][sx]
                    wgt = wx * wy
                    acc[0] += r * a * wgt
                    acc[1] += g * a * wgt
                    acc[2] += b * a * wgt
                    acc[3] += a * wgt
                    total += wgt
            a = acc[3] / total
            if acc[3] > 0:
                row.append((round(acc[0] / acc[3]), round(acc[1] / acc[3]),
                            round(acc[2] / acc[3]), round(a)))
            else:
                row.append((0, 0, 0, 0))
        out.append(row)
    return out

def write_bmp24(path, size, rows):
    """rows: lists of (r, g, b); bottom-up 24-bit BMP."""
    stride = (size * 3 + 3) & ~3
    data = bytearray()
    for row in reversed(rows):
        line = bytearray()
        for r, g, b in row:
            line += bytes((b, g, r))
        line += b'\0' * (stride - len(line))
        data += line
    header = struct.pack('<2sIHHI', b'BM', 54 + len(data), 0, 0, 54)
    info = struct.pack('<IiiHHIIiiII', 40, size, size, 1, 24, 0, len(data), 2835, 2835, 0, 0)
    open(path, 'wb').write(header + info + data)

def main():
    xpm, outdir = sys.argv[1], sys.argv[2]
    size = int(sys.argv[3]) if len(sys.argv) > 3 else 88
    w, h, px = read_xpm(xpm)
    n, sq = to_square(w, h, px)
    icon = downscale(n, sq, size)
    os.makedirs(outdir, exist_ok=True)
    write_bmp24(os.path.join(outdir, 'netsurf.bmp'), size,
                [[p[:3] for p in row] for row in icon])
    write_bmp24(os.path.join(outdir, 'netsurf_mask_soft.bmp'), size,
                [[(p[3],) * 3 for p in row] for row in icon])
    print('icon: %dx%d from %s' % (size, size, os.path.basename(xpm)))

if __name__ == '__main__':
    main()
