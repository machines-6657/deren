"""Does the glossy reflection survive camera motion? A difference of differences.

Usage: python motion_dd.py <sweep_on.png> <static_on.png> <sweep_off.png> <static_off.png>

A moving temporal accumulation differs from a converged static one even when every reprojection is
perfect - it is an average over the motion - so "the swept frame differs from the still one" is not by
itself an error. What isolates the REFLECTION's share of it is a difference of differences: take
`sweep - static` with the glossy lobe ON and with it OFF, and what survives subtracting one from the
other is the motion loss only the reflection has.

THE FOUR ARMS HAVE TO SHARE A POSE, and the one that is easy to get wrong is the static pair. The swept
arm's captured frame is rendered at `yaw = base + sweep_deg_per_frame * frames` (the pose convention in
the pose convention these arms were established with, by a control), so the static arm has to
be captured at THAT pose:

    capture.ps1 -Base <the scenario's config> -Camera ""            -Sweep 0.5 -Frames 40   # swept
    capture.ps1 -Base <the same config>        -Camera "20,0,19.24"              -Frames 40   # static

where `20` is the base yaw plus 20 degrees of orbit, and the base pose is read from the run's own log
line - `initial camera: fit=exterior yaw 0.0 deg, pitch 0.0 deg, distance 19.24 (scene radius 6.99 ...)`.
A static arm taken at the BASE pose instead measures the 20-degree orbit rather than the reprojection,
and it is a plausible-looking number rather than an obviously wrong one. Two further traps, both of them
this repository's: `capture.ps1`'s own `-Camera` DEFAULT is Sponza's interior pose, so the swept arm
needs `-Camera ""` to leave the pose to the scene's `camera_fit`; and `-Base` resolves against
`-WorkDir`, so a config outside the work directory has to be copied into it.

Green channel throughout, like every other number in these records. The recorded arms are in
The recorded arms: the reflection's own loss measured 0.4029 before the
accumulation cap and 0.3492 after it, with the change confined to the smooth-material tiles and the
rough ones coming out bit-identical.
"""
import sys
from PIL import Image


def green(path):
    """The frame's green channel as raw bytes (the channel every number here is quoted in)."""
    im = Image.open(path).convert("RGBA")
    return im.width, im.height, im.tobytes()


def main():
    if len(sys.argv) != 5:
        print(__doc__)
        return 1
    sweep_on, static_on, sweep_off, static_off = (green(path) for path in sys.argv[1:5])
    w, h, _ = sweep_on
    for name, img in (("static_on", static_on), ("sweep_off", sweep_off), ("static_off", static_off)):
        if (img[0], img[1]) != (w, h):
            print(f"extent mismatch: {w}x{h} vs {img[0]}x{img[1]} ({name})")
            return 1

    n = w * h
    arm_on = 0
    arm_off = 0
    abs_dd = 0
    sum_dd = 0.0
    tiles = [[0.0] * 4 for _ in range(4)]
    tile_n = [[0] * 4 for _ in range(4)]
    for y in range(h):
        ty = y * 4 // h
        for x in range(w):
            i = (y * w + x) * 4
            tx = x * 4 // w
            d_on = sweep_on[2][i + 1] - static_on[2][i + 1]
            d_off = sweep_off[2][i + 1] - static_off[2][i + 1]
            arm_on += abs(d_on)
            arm_off += abs(d_off)
            dd = d_on - d_off
            abs_dd += abs(dd)
            sum_dd += dd
            tiles[ty][tx] += dd
            tile_n[ty][tx] += 1

    worst = max((abs(tiles[ty][tx] / tile_n[ty][tx]), ty, tx) for ty in range(4) for tx in range(4))
    print(f"  sweep_on   {sys.argv[1]}")
    print(f"  static_on  {sys.argv[2]}   (must be the sweep's END pose)")
    print(f"  sweep_off  {sys.argv[3]}")
    print(f"  static_off {sys.argv[4]}")
    print(f"  mean|sweep - static|  lobe ON  {arm_on/n:.4f}")
    print(f"  mean|sweep - static|  lobe OFF {arm_off/n:.4f}")
    print(f"  mean|DD|                      {abs_dd/n:.4f}   <- the reflection's own motion loss")
    print(f"  mean DD                       {sum_dd/n:+.4f}")
    print("  4x4 tiles of mean signed double difference:")
    for ty in range(4):
        print("    " + "  ".join(f"{tiles[ty][tx]/tile_n[ty][tx]:+7.3f}" for tx in range(4)))
    print(f"  worst tile {worst[0]:.3f} at row {worst[1]}, column {worst[2]}")
    return 0


sys.exit(main())
