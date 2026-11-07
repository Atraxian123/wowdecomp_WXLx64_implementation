#!/usr/bin/env python3
"""Writes PNG test cases (and the RGBA pixels each must decode to) for png_to_blp_test.cpp.
Own encoder so every color type / bit depth / filter / Adam7 interlace is covered, not only what Pillow writes."""
import os, random, struct, sys, zlib

out = sys.argv[1]
os.makedirs(out, exist_ok=True)
rnd = random.Random(1234)

def chunk(t, d):
    return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)

def paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    return a if pa <= pb and pa <= pc else (b if pb <= pc else c)

def filter_rows(rows, bpp):
    res, prev = bytearray(), bytes(len(rows[0])) if rows else b""
    for y, row in enumerate(rows):
        ft = (y + len(rows)) % 5                       # every filter type shows up
        res.append(ft)
        for i, x in enumerate(row):
            a = row[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            pred = (0, a, b, (a + b) >> 1, paeth(a, b, c))[ft]
            res.append((x - pred) & 255)
        prev = row
    return bytes(res)

def pack_row(samples, depth):
    if depth == 8:
        return bytes(samples)
    if depth == 16:
        return b"".join(struct.pack(">H", s) for s in samples)
    bits = "".join(format(s, "0%db" % depth) for s in samples)
    bits += "0" * (-len(bits) % 8)
    return bytes(int(bits[i:i + 8], 2) for i in range(0, len(bits), 8))

CH = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}

def expected_pixel(ct, depth, s, plte, trns):
    mx = (1 << depth) - 1
    def to8(v): return v >> 8 if depth == 16 else (v * 255 // mx if depth < 8 else v)
    if ct == 0:
        a = 0 if trns is not None and s[0] == trns[0] else 255
        return (to8(s[0]),) * 3 + (a,)
    if ct == 2:
        a = 0 if trns is not None and tuple(s) == tuple(trns) else 255
        return (to8(s[0]), to8(s[1]), to8(s[2]), a)
    if ct == 3:
        r, g, b = plte[s[0]]
        return (r, g, b, trns[s[0]] if trns is not None and s[0] < len(trns) else 255)
    if ct == 4:
        return (to8(s[0]),) * 3 + (to8(s[1]),)
    return tuple(to8(v) for v in s)

def make(name, w, h, ct, depth, interlace=0, trns=None, plte=None):
    ch = CH[ct]
    mx = (1 << depth) - 1
    if ct == 3:
        plte = plte or [(rnd.randrange(256), rnd.randrange(256), rnd.randrange(256)) for _ in range(min(256, mx + 1))]
    img = [[[rnd.randrange(mx + 1) if ct != 3 else rnd.randrange(len(plte)) for _ in range(ch)] for _ in range(w)] for _ in range(h)]
    if trns is not None and ct in (0, 2):                  # make sure the key color actually occurs
        img[0][0] = list(trns)
    bpp = max(1, ch * depth // 8)
    if not interlace:
        rows = [pack_row([v for px in r for v in px], depth) for r in img]
        raw = filter_rows(rows, bpp) if rows else b""
    else:
        raw = b""
        X0, Y0, DX, DY = (0, 4, 0, 2, 0, 1, 0), (0, 0, 4, 0, 2, 0, 1), (8, 8, 4, 4, 2, 2, 1), (8, 8, 8, 4, 4, 2, 2)
        for i in range(7):
            ys, xs = range(Y0[i], h, DY[i]), range(X0[i], w, DX[i])
            if not len(ys) or not len(xs):
                continue
            rows = [pack_row([v for x in xs for v in img[y][x]], depth) for y in ys]
            raw += filter_rows(rows, bpp)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, depth, ct, 0, 0, interlace))
    if ct == 3:
        png += chunk(b"PLTE", b"".join(bytes(c) for c in plte))
    if trns is not None:
        if ct == 3: png += chunk(b"tRNS", bytes(trns))
        elif ct == 0: png += chunk(b"tRNS", struct.pack(">H", trns[0]))
        elif ct == 2: png += chunk(b"tRNS", struct.pack(">HHH", *trns))
    half = len(raw) // 2 or 1                              # two IDATs: chunk joining is exercised too
    z = zlib.compress(raw, 6)
    png += chunk(b"IDAT", z[:half]) + chunk(b"IDAT", z[half:]) + chunk(b"IEND", b"")
    open(os.path.join(out, name + ".png"), "wb").write(png)
    exp = bytearray()
    for r in img:
        for px in r:
            exp += bytes(expected_pixel(ct, depth, px, plte, trns))
    open(os.path.join(out, name + ".rgba"), "wb").write(bytes(exp))
    open(os.path.join(out, name + ".txt"), "w").write("%d %d\n" % (w, h))

for il in (0, 1):
    t = "_i" if il else ""
    for d in (1, 2, 4, 8, 16): make("gray%d%s" % (d, t), 13, 11, 0, d, il)
    for d in (8, 16): make("rgb%d%s" % (d, t), 16, 8, 2, d, il)
    for d in (1, 2, 4, 8): make("pal%d%s" % (d, t), 9, 17, 3, d, il)
    for d in (8, 16): make("graya%d%s" % (d, t), 7, 5, 4, d, il)
    for d in (8, 16): make("rgba%d%s" % (d, t), 32, 16, 6, d, il)
    make("palTrns%s" % t, 8, 8, 3, 8, il, trns=[0, 128, 255, 7])
    make("grayTrns%s" % t, 10, 6, 0, 8, il, trns=[3])
    make("rgbTrns%s" % t, 10, 6, 2, 8, il, trns=[1, 2, 3])
make("tiny1x1", 1, 1, 6, 8)
make("pot64", 64, 64, 6, 8)
make("npot", 30, 20, 2, 8)
# a file Pillow writes, for a reading the encoder above cannot have shaped
try:
    from PIL import Image
    im = Image.new("RGBA", (16, 16))
    im.putdata([(x * 16, y * 16, (x + y) * 8, 255 if (x + y) % 3 else 0) for y in range(16) for x in range(16)])
    im.save(os.path.join(out, "pillow_rgba.png"), optimize=True)
    open(os.path.join(out, "pillow_rgba.rgba"), "wb").write(im.tobytes())
    open(os.path.join(out, "pillow_rgba.txt"), "w").write("16 16\n")
except ImportError:
    pass
print("cases written to", out)
