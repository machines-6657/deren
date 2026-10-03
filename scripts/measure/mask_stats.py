"""Is the mask bake moving the ray-traced shadow TOWARDS the raster one?

Usage: python mask_stats.py <cascade.png> <rt_nobake.png> <rt_baked.png>

The three captures are the same scene and camera with (1) raster shadows, which discard MASK
fragments per pixel and are therefore the reference, (2) ray-traced shadows with the mask baked
nowhere, i.e. solid, and (3) ray-traced shadows with the bake on.

Prints the frame means, the mean ABSOLUTE difference of each traced frame against the reference
(the error the bake is supposed to shrink), and the correlation between "what the bake changed"
and "how wrong the unbaked frame was" - which is the part that says whether it changed it in the
right DIRECTION rather than merely by a lot.
"""
import sys
from PIL import Image


def green(path):
    im = Image.open(path).convert("RGBA")
    data = im.tobytes()
    return im.width, im.height, [data[i + 1] for i in range(0, len(data), 4)]


w, h, cascade = green(sys.argv[1])
_, _, nobake = green(sys.argv[2])
_, _, baked = green(sys.argv[3])
n = w * h
mean = lambda c: sum(c) / n

err_before = [cascade[i] - nobake[i] for i in range(n)]
err_after = [cascade[i] - baked[i] for i in range(n)]
effect = [baked[i] - nobake[i] for i in range(n)]


def pearson(x, y):
    mx, my = mean(x), mean(y)
    sxy = sxx = syy = 0.0
    for i in range(n):
        dx, dy = x[i] - mx, y[i] - my
        sxy += dx * dy
        sxx += dx * dx
        syy += dy * dy
    return sxy / (sxx ** 0.5 * syy ** 0.5) if sxx > 0 and syy > 0 else 0.0


def table(values, label):
    print(f"  {label}: mean {mean(values):+.4f}  mean|.| {sum(abs(v) for v in values)/n:.4f}")
    for ty in range(4):
        row = []
        for tx in range(4):
            s = c = 0
            for y in range(ty * h // 4, (ty + 1) * h // 4):
                base = y * w
                for x in range(tx * w // 4, (tx + 1) * w // 4):
                    s += values[base + x]
                    c += 1
            row.append(f"{s/c:+6.2f}")
        print("    " + "  ".join(row))


print(f"frame means: raster {mean(cascade):.4f}  rt solid {mean(nobake):.4f}  rt baked {mean(baked):.4f}")
table(err_before, "error vs raster, mask solid")
table(err_after, "error vs raster, mask baked")
print(f"  the bake's own effect on the frame: mean {mean(effect):+.4f}, mean|.| {sum(abs(v) for v in effect)/n:.4f}, "
      f"{sum(1 for v in effect if v != 0)} pixels changed")
print(f"  correlation(effect, how wrong the solid frame was) = {pearson(effect, err_before):+.4f}")
print(f"  pixels where the bake moved TOWARDS the raster: "
      f"{sum(1 for i in range(n) if effect[i] != 0 and abs(err_after[i]) < abs(err_before[i]))} of "
      f"{sum(1 for i in range(n) if effect[i] != 0)} changed")
