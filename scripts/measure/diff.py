"""Per-pixel difference statistics between two captured frames.

Usage: python diff.py <a.png> <b.png>

Prints the mean signed difference (a - b) per channel, how many pixels differ at all
and by more than 1, and a 4x4 tile table of the mean difference - the instrument that
shows whether a frame-level difference is a global bias or is ordered by the scene.

READ THE COUNTS AS GREEN-ONLY, because that is what they are: "differing", "|d|>1",
"|d|>4" and `max |d|` all test the GREEN channel's difference alone, so a pixel that
changed only in red or blue is invisible to those four numbers while the per-channel
means still move. Measured, on a real capture pair: this reported "0 pixels differing"
for a frame whose decoded arrays differ in one BLUE texel by one step. The means can be
trusted; the counts cannot be read as "no pixel changed", and every count this project
recorded before that was noticed inherits the caveat. Not changed here on purpose -
making the counter all-channel would silently invalidate those recorded numbers against
their own record.
"""
import sys
from PIL import Image


def channels(path):
    im = Image.open(path).convert("RGBA")
    data = im.tobytes()
    return im.width, im.height, data


wa, ha, a = channels(sys.argv[1])
wb, hb, b = channels(sys.argv[2])
if (wa, ha) != (wb, hb):
    print(f"extent mismatch: {wa}x{ha} vs {wb}x{hb}")
    sys.exit(1)

n = wa * ha
sums = [0, 0, 0]
nonzero = 0
over1 = 0
over4 = 0
mx = 0
tiles = [[0.0] * 4 for _ in range(4)]
tile_n = [[0] * 4 for _ in range(4)]
for y in range(ha):
    ty = y * 4 // ha
    for x in range(wa):
        i = (y * wa + x) * 4
        tx = x * 4 // wa
        d = (a[i + 1] - b[i + 1])  # green, the channel every number here is quoted in
        sums[0] += a[i] - b[i]
        sums[1] += d
        sums[2] += a[i + 2] - b[i + 2]
        if d != 0:
            nonzero += 1
        ad = abs(d)
        if ad > 1:
            over1 += 1
        if ad > 4:
            over4 += 1
        if ad > mx:
            mx = ad
        tiles[ty][tx] += d
        tile_n[ty][tx] += 1

print(f"{sys.argv[1]}  -  {sys.argv[2]}   ({wa}x{ha})")
print(f"  mean diff    R {sums[0]/n:+.4f}  G {sums[1]/n:+.4f}  B {sums[2]/n:+.4f}")
print(f"  pixels       differing {nonzero} ({100.0*nonzero/n:.2f}%)   |d|>1 {over1} ({100.0*over1/n:.2f}%)   |d|>4 {over4} ({100.0*over4/n:.2f}%)   max |d| {mx}")
print("  4x4 tiles of mean green difference:")
for ty in range(4):
    print("    " + "  ".join(f"{tiles[ty][tx]/tile_n[ty][tx]:+7.3f}" for tx in range(4)))
