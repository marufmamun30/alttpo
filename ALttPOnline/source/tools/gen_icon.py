# Draws the program icon (a Triforce on a dark blue shield) and writes res/alttpo.ico
import struct, sys, zlib

def make(size):
    px = [[(0, 0, 0, 0)] * size for _ in range(size)]
    cx = size / 2.0
    # rounded dark blue badge with a gold rim
    for y in range(size):
        for x in range(size):
            dx, dy = (x + 0.5 - cx) / cx, (y + 0.5 - cx) / cx
            d = max(abs(dx), abs(dy)) * 0.55 + (dx * dx + dy * dy) ** 0.5 * 0.45
            if d < 0.90:
                px[y][x] = (16, 24, 72, 255)
            elif d < 0.98:
                px[y][x] = (200, 144, 32, 255)
    def tri(apx, top, h):
        for r in range(int(h)):
            half = r * 0.58
            for x in range(int(apx - half), int(apx + half) + 1):
                yy = int(top + r)
                if 0 <= x < size and 0 <= yy < size:
                    t = (x - (apx - half)) / (2 * half + 1)
                    c = (255, 240, 150) if t < 0.25 else ((248, 208, 56) if t < 0.75 else (184, 124, 24))
                    px[yy][x] = c + (255,)
    h = size * 0.31
    top = size * 0.17
    tri(cx, top, h)
    tri(cx - h * 0.58, top + h, h)
    tri(cx + h * 0.58, top + h, h)
    return px

def png(px):
    size = len(px)
    raw = b"".join(b"\x00" + b"".join(struct.pack("BBBB", *p) for p in row) for row in px)
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")

sizes = [16, 32, 48, 256]
imgs = [png(make(s)) for s in sizes]
out = struct.pack("<HHH", 0, 1, len(sizes))
off = 6 + 16 * len(sizes)
for s, d in zip(sizes, imgs):
    out += struct.pack("<BBBBHHII", s % 256, s % 256, 0, 0, 1, 32, len(d), off)
    off += len(d)
out += b"".join(imgs)
open(sys.argv[1], "wb").write(out)
print("icon written", len(out))
