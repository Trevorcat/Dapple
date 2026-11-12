# Synthetic test photo (no private data): sky/ground gradient, sun, colour chart patches, grain. Pure python PNG -> `sips` JPEG.
import math, random, struct, sys, zlib
W, H = 1800, 1200
random.seed(7)
rows = []
for y in range(H):
    r = bytearray([0])
    for x in range(W):
        t = y / H
        if t < 0.55:
            c = (60 + 120 * t, 110 + 130 * t, 200 + 40 * t)
        else:
            u = (t - 0.55) / 0.45
            c = (90 - 40 * u + 20 * math.sin(x / 90), 120 - 50 * u, 60 - 30 * u)
        d = math.hypot(x - 1300, y - 260)
        if d < 90: c = (255, 245, 210)
        elif d < 260: k = 1 - (d - 90) / 170; c = tuple(min(255, v + 90 * k * k) for v in c)
        for i in range(6):
            if 80 + i * 130 <= x < 190 + i * 130 and 860 <= y < 1000:
                c = [(200, 40, 40), (230, 170, 40), (60, 170, 70), (40, 160, 200), (70, 70, 190), (190, 70, 170)][i]
        n = random.gauss(0, 3)
        r += bytes(max(0, min(255, int(v + n))) for v in c)
    rows.append(bytes(r))
raw = b''.join(rows)
def chunk(t, d): c = struct.pack('>I', len(d)) + t + d; return c + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
open(sys.argv[1], 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))
