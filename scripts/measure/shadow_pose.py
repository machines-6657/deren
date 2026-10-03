"""Does an ANIMATED SKINNED mesh's ray-traced shadow follow the pose?

Usage: python shadow_pose.py <raster_t0.png> <raster_t1.png> <rt_t0.png> <rt_t1.png>

The four captures are the same animated model at two pinned poses (and with raster vs ray-traced
shadows). The OBJECT moves in all four, so a plain frame difference says little; the instrument is
the difference OF the differences. What is left after subtracting

    d_raster = raster(t0) - raster(t1)      object motion + the raster shadow following the pose
    d_rt     = rt(t0)     - rt(t1)          object motion + whatever the traced shadow does

is d = d_raster - d_rt: the part of the pose's effect that only the RASTER path has, i.e. the
shadow's own pose dependence. A traced shadow that follows the pose makes d collapse to the usual
traced-vs-cascade difference; a traced shadow stuck in the bind pose leaves d as large as the
shadow's motion, which is the limitation this measures.
"""
import sys
from PIL import Image


def green(path):
    im = Image.open(path).convert("RGBA")
    data = im.tobytes()
    return im.width, im.height, [data[i + 1] for i in range(0, len(data), 4)]


w, h, raster0 = green(sys.argv[1])
_, _, raster1 = green(sys.argv[2])
_, _, rt0 = green(sys.argv[3])
_, _, rt1 = green(sys.argv[4])
n = w * h
mean = lambda c: sum(c) / n

d_raster = [raster0[i] - raster1[i] for i in range(n)]
d_rt = [rt0[i] - rt1[i] for i in range(n)]
d = [d_raster[i] - d_rt[i] for i in range(n)]

print(f"  raster shadows: poser0 - pose1   mean {mean(d_raster):+.4f}   mean|.| {sum(abs(v) for v in d_raster)/n:.4f}")
print(f"  traced shadows: poser0 - pose1   mean {mean(d_rt):+.4f}   mean|.| {sum(abs(v) for v in d_rt)/n:.4f}")
print(f"  ONLY the raster path has (d):    mean {mean(d):+.4f}   mean|.| {sum(abs(v) for v in d)/n:.4f}")
print("  4x4 tiles of d (the pose dependence only the raster shadow has):")
for ty in range(4):
    row = []
    for tx in range(4):
        s = c = 0
        for y in range(ty * h // 4, (ty + 1) * h // 4):
            base = y * w
            for x in range(tx * w // 4, (tx + 1) * w // 4):
                s += d[base + x]
                c += 1
        row.append(f"{s/c:+7.3f}")
    print("    " + "  ".join(row))
print(f"  pixels where the raster shadow moved and the traced one did not: "
      f"{sum(1 for i in range(n) if abs(d_raster[i]) > 2 and abs(d_rt[i]) <= 1)}")
