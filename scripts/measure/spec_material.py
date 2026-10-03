import sys
from PIL import Image


def channel(path, idx):
    im = Image.open(path).convert("RGBA")
    data = im.tobytes()
    return [data[i + idx] for i in range(0, len(data), 4)]


on_g = channel(sys.argv[1], 1)
off_g = channel(sys.argv[2], 1)
rough = channel(sys.argv[3], 0)
metal = channel(sys.argv[4], 0)
normal = channel(sys.argv[5], 0)  # channel 1 is normal*0.5+0.5, so all-zero is the only background value

rough_buckets = [(0, 8), (8, 32), (32, 64), (64, 128), (128, 256)]
metal_buckets = [(0, 8), (8, 32), (32, 64), (64, 128), (128, 256)]

# Background pixels are where the debug view showed nothing: the cleared depth paints EVERY channel
# black (gbuffer_debug.frag). Testing "roughness == 0 and metallic == 0" would also drop the
# smooth-dielectric sphere, which is exactly one of the corners this table is about; a unit normal can
# never map to (0,0,0) through normal*0.5+0.5, so the normal channel is the mask that cannot lie.
surface = [i for i in range(len(on_g)) if normal[i] != 0]
print(f"surface pixels: {len(surface)} of {len(on_g)}")

print("\ndelta green by ROUGHNESS bucket (display units: the debug view is tonemapped, so these are")
print("ordinal, not linear roughness - the mapping is monotone and that is all the bucket needs)")
for lo, hi in rough_buckets:
    sel = [i for i in surface if lo <= rough[i] < hi]
    if not sel:
        print(f"  rough [{lo:3d},{hi:3d})   no pixels")
        continue
    d = sum(on_g[i] - off_g[i] for i in sel) / len(sel)
    print(f"  rough [{lo:3d},{hi:3d})   {len(sel):7d} px   mean delta {d:+7.4f}")

print("\ndelta green by METALLIC bucket")
for lo, hi in metal_buckets:
    sel = [i for i in surface if lo <= metal[i] < hi]
    if not sel:
        print(f"  metal [{lo:3d},{hi:3d})   no pixels")
        continue
    d = sum(on_g[i] - off_g[i] for i in sel) / len(sel)
    print(f"  metal [{lo:3d},{hi:3d})   {len(sel):7d} px   mean delta {d:+7.4f}")

print("\ndelta green by the four corners of the material grid (display units)")
for rlo, rhi, rname in [(0, 32, "smooth"), (128, 256, "rough")]:
    for mlo, mhi, mname in [(0, 32, "dielectric"), (128, 256, "metal")]:
        sel = [i for i in surface if rlo <= rough[i] < rhi and mlo <= metal[i] < mhi]
        if not sel:
            print(f"  {rname:6s} {mname:10s}  no pixels")
            continue
        d = sum(on_g[i] - off_g[i] for i in sel) / len(sel)
        print(f"  {rname:6s} {mname:10s}  {len(sel):7d} px   mean delta {d:+7.4f}")
