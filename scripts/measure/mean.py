"""Mean 8-bit channel statistics for captured frames, the instrument the furnace
acceptance is read with. Usage: python mean.py <png> [<png> ...]

Prints, per image: mean green (the number the acceptance is quoted in), mean of
each channel, and the mean luminance used by the tertile tables.
"""
import sys
from PIL import Image


def stats(path):
    im = Image.open(path).convert("RGBA")
    px = im.tobytes()
    n = im.width * im.height
    sums = [0, 0, 0]
    for i in range(0, len(px), 4):
        sums[0] += px[i]
        sums[1] += px[i + 1]
        sums[2] += px[i + 2]
    return n, [s / n for s in sums]


for path in sys.argv[1:]:
    n, (r, g, b) = stats(path)
    print(f"{path}\n  {n} px  mean R {r:.4f}  G {g:.4f}  B {b:.4f}")
