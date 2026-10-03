"""Correlate two per-pixel difference images.

Usage: python corr.py <a_on.png> <a_off.png> <b_on.png> <b_off.png>

Builds dA = a_on - a_off and dB = b_on - b_off on the green channel (the channel every
number in this work is quoted in), then reports the Pearson correlation and each one's
4x4 tile table. Independent estimators of the same quantity should correlate; a
frame-level coincidence of means without correlation is not the same thing.
"""
import sys
from PIL import Image


def green(path):
    im = Image.open(path).convert("RGBA")
    data = im.tobytes()
    return im.width, im.height, [data[i + 1] for i in range(0, len(data), 4)]


wa, ha, a1 = green(sys.argv[1])
_, _, a0 = green(sys.argv[2])
_, _, b1 = green(sys.argv[3])
_, _, b0 = green(sys.argv[4])

n = wa * ha
da = [a1[i] - a0[i] for i in range(n)]
db = [b1[i] - b0[i] for i in range(n)]


def pearson(x, y):
    mx = sum(x) / n
    my = sum(y) / n
    sxy = sxx = syy = 0.0
    for i in range(n):
        dx = x[i] - mx
        dy = y[i] - my
        sxy += dx * dy
        sxx += dx * dx
        syy += dy * dy
    if sxx <= 0.0 or syy <= 0.0:
        return 0.0
    return sxy / (sxx ** 0.5 * syy ** 0.5)


def table(d, label):
    print(f"  {label}: mean {sum(d)/n:+.4f}")
    for ty in range(4):
        row = []
        for tx in range(4):
            s = 0
            c = 0
            for y in range(ty * ha // 4, (ty + 1) * ha // 4):
                base = y * wa
                for x in range(tx * wa // 4, (tx + 1) * wa // 4):
                    s += d[base + x]
                    c += 1
            row.append(f"{s/c:+7.3f}")
        print("    " + "  ".join(row))


print(f"A: {sys.argv[1]} - {sys.argv[2]}   vs   B: {sys.argv[3]} - {sys.argv[4]}")
print(f"  pearson r = {pearson(da, db):+.4f}")
table(da, "A")
table(db, "B")
