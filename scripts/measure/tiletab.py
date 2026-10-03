"""Per-tile cache effect, in the units the earlier records quote it.

Usage: python tiletab.py <with_cache.png> <without_cache.png>

Prints a 4x4 table of the green-channel difference (with - without) for each tile, both
as an absolute mean and as a percentage of that tile's own mean in the "without" frame -
which is the form that shows whether an effect is ordered by the scene or by the material.
"""
import sys
from PIL import Image


def green(path):
    im = Image.open(path).convert("RGBA")
    data = im.tobytes()
    return im.width, im.height, [data[i + 1] for i in range(0, len(data), 4)]


w, h, on = green(sys.argv[1])
_, _, off = green(sys.argv[2])
n = w * h

print(f"{sys.argv[1]} - {sys.argv[2]}   ({w}x{h})")
print(f"  frame mean: {sum(on)/n:.4f} - {sum(off)/n:.4f} = {sum(on)/n - sum(off)/n:+.4f}")
print("  per tile: absolute mean difference and % of that tile's own 'without' mean")
for ty in range(4):
    abs_row = []
    pct_row = []
    for tx in range(4):
        s = 0
        base = 0
        c = 0
        for y in range(ty * h // 4, (ty + 1) * h // 4):
            row = y * w
            for x in range(tx * w // 4, (tx + 1) * w // 4):
                s += on[row + x] - off[row + x]
                base += off[row + x]
                c += 1
        abs_row.append(f"{s/c:+7.3f}")
        pct_row.append(f"{100.0 * s / max(base, 1):+6.2f}%")
    print("    " + "  ".join(abs_row))
    print("    " + "  ".join(pct_row))
