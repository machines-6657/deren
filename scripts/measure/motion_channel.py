#!/usr/bin/env python3
"""Measure the G-buffer MOTION channel: did the pixels that deform actually report motion?

WHY THIS EXISTS. `gbuffer_debug` channel 8 draws TAA's motion vector as
`clamp(motion * motion_gain + 0.5, 0, 1)` with `motion_gain = width / 4` (four pixels of motion
saturate), and it renders through the post chain, so the numbers here are display-encoded rather than
UV units. That is fine for what this measures, which is a question of PRESENCE, not of exact value:
of the pixels that carry geometry, how many report that they did not move?

The reference to compare against is the same capture of a STATIC scene: a static camera and a static
node make the motion vector exactly zero, so its foreground is one flat value - that value IS "did not
move" after the post chain, and it is read from the image rather than assumed (it lands at 206 on the
RTX 4060 at 1080x960, and hard-coding that would be a number that breaks on the next driver).

So the reading is:

  * `foreground`  - pixels that carry geometry (B == 0 and not the black background);
  * `still`       - the share of them that report the still constant, i.e. "did not move";
  * `moved`       - the share that report anything else;
  * `mean |d|`    - the mean absolute deviation from that constant, in 8-bit units.

A rigid scene must read 100% still (TAA off). A DEFORMING scene read 100% still before
deformation-aware motion vectors existed - that is the bug this instrument was written to see - and
must read a clearly non-zero `moved` after, while a PINNED pose (same matrices every frame) must still
read 100% still: that is the null test which says the new path can be zero.

Usage:
    python scripts/measure/motion_channel.py <motion_on.png> <motion_static_reference.png>
"""

from __future__ import annotations

import sys
from collections import Counter

from PIL import Image


def read(path: str) -> tuple[Counter, int, int]:
    """Return ((R, G) histogram over foreground pixels, foreground count, total count).

    Foreground = the geometry the debug view shaded: channel 8 writes B = 0 everywhere, so the
    background (no geometry) is exactly black and would otherwise dominate the histogram."""
    image = Image.open(path).convert("RGB")
    width, height = image.size
    pixels = image.load()
    histogram: Counter = Counter()
    foreground = 0
    total = 0
    for y in range(height):
        for x in range(width):
            red, green, blue = pixels[x, y]
            total += 1
            if blue != 0 or (red == 0 and green == 0):
                continue
            histogram[(red, green)] += 1
            foreground += 1
    return histogram, foreground, total


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__.strip().splitlines()[-1], file=sys.stderr)
        return 2

    on_path, reference_path = sys.argv[1], sys.argv[2]
    reference, reference_foreground, _ = read(reference_path)
    if not reference or not reference_foreground:
        print(f"{reference_path}: no foreground pixels - is gbuffer_debug + gbuffer_channel = 8 set?", file=sys.stderr)
        return 1

    # the still constant is the reference image's dominant foreground value: a static scene reports
    # exactly one of them, and taking the dominant one keeps this working if a stray pixel differs
    (still_r, still_g), still_count = reference.most_common(1)[0]
    if still_count != reference_foreground:
        print(
            f"warning: the reference is not uniformly still "
            f"({reference_foreground - still_count} of {reference_foreground} foreground pixels differ) - "
            f"a moving camera or a deforming node in the reference makes 'still' ambiguous",
            file=sys.stderr,
        )

    on, on_foreground, _ = read(on_path)
    moved = 0
    deviation = 0
    worst = 0
    for (red, green), count in on.items():
        delta = max(abs(red - still_r), abs(green - still_g))
        if delta > 1:
            moved += count
        deviation += delta * count
        worst = max(worst, delta)

    print(f"still constant (from {reference_path}): ({still_r}, {still_g})")
    print(f"{on_path}:")
    print(f"  foreground       {on_foreground} px")
    print(f"  still            {on_foreground - moved} px  ({(on_foreground - moved) / max(on_foreground, 1) * 100:.2f}%)")
    print(f"  moved            {moved} px  ({moved / max(on_foreground, 1) * 100:.2f}%)")
    print(f"  mean |deviation| {deviation / max(on_foreground, 1):.3f} 8-bit units")
    print(f"  worst deviation  {worst} 8-bit units")
    print(f"  distinct values  {len(on)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
