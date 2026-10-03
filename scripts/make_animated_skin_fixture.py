#!/usr/bin/env python3
"""Generate tests/fixtures/animated_skin_plane.gltf - a DEFORMING asset for the motion-vector tests.

Why this asset exists, and why it is generated rather than hand-written. Every other model this
repository ships is rigid, and a rigid model cannot show the one thing this fixture is for: a vertex
that moves inside its own object space. `shared_skin_two_nodes.gltf` has a skin but no animation
channel, so it never deforms either. A hand-written plane grid with per-vertex weights is a base64
blob nobody can review, so the geometry is emitted from here instead - the same reason
scripts/bake_toon_ramp.py exists on the NPR branch.

THE SHAPE OF THE TEST, which is why the asset is a swinging flag rather than a rotating cube:

  * the mesh node is STATIC and the camera is static, so the RIGID half of the motion vector is
    exactly zero everywhere - the only correct velocity on this asset is the deformation term;
  * the left edge is pinned to a static joint, so the deformation is in the IMAGE PLANE (a swing
    about Z), which means it moves pixels rather than just depth;
  * the weights blend smoothly across x, so the surface shears rather than tearing.

So a capture of this asset separates the two cases cleanly: before deformation-aware motion vectors
the whole swinging half reports "did not move" (the flat olive of gbuffer_debug's motion channel),
and after it reports the motion it actually has.

Usage:
    python scripts/make_animated_skin_fixture.py                  # write the committed fixture
    python scripts/make_animated_skin_fixture.py --grid 24 --out /tmp/big.gltf
"""

from __future__ import annotations

import argparse
import base64
import json
import math
import struct
from pathlib import Path

# glTF component types
FLOAT = 5126
UNSIGNED_BYTE = 5121
UNSIGNED_SHORT = 5123

# the swing: keyframe times in seconds and the Z angle at each, in degrees. A full back-and-forth
# over 2 s keeps the loop short enough that a 40-frame capture at 0.02 s/frame lands mid-swing.
KEY_TIMES = [0.0, 0.5, 1.0, 1.5, 2.0]
KEY_ANGLES_DEG = [0.0, 35.0, 0.0, -35.0, 0.0]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--grid", type=int, default=12, help="quads per side (default 12: 169 vertices, 288 triangles)")
    parser.add_argument("--size", type=float, default=2.0, help="plane side length in scene units (default 2.0)")
    parser.add_argument(
        "--out",
        type=Path,
        default=Path(__file__).resolve().parent.parent / "tests" / "fixtures" / "animated_skin_plane.gltf",
        help="output .gltf path (default: tests/fixtures/animated_skin_plane.gltf)",
    )
    return parser.parse_args()


class Buffer:
    """An append-only byte buffer that records the byteOffset of every block it is handed."""

    def __init__(self) -> None:
        self.data = bytearray()
        self.views: list[tuple[int, int]] = []  # (byteOffset, byteLength)

    def add(self, raw: bytes, stride_align: int = 4) -> int:
        """Append @p raw 4-byte aligned (so every float accessor starts on a float boundary) and
        return the index of the bufferView that describes it."""
        while len(self.data) % stride_align != 0:
            self.data.append(0)
        offset = len(self.data)
        self.data.extend(raw)
        self.views.append((offset, len(raw)))
        return len(self.views) - 1


def build(args: argparse.Namespace) -> dict:
    grid = args.grid
    side = args.size
    half = side * 0.5
    step = side / grid

    # ---- geometry: a (grid+1)^2 vertex grid in the XY plane, normal +Z, x from -half to +half ----
    positions: list[tuple[float, float, float]] = []
    normals: list[tuple[float, float, float]] = []
    joints: list[tuple[int, int, int, int]] = []
    weights: list[tuple[float, float, float, float]] = []
    for row in range(grid + 1):
        y = -half + row * step
        for col in range(grid + 1):
            x = -half + col * step
            positions.append((x, y, 0.0))
            normals.append((0.0, 0.0, 1.0))
            # smoothstep over x in [-0.2, 0.2]: the right half rides the animated joint, the left
            # half is pinned to the static one, and the band between them shears.
            t = min(1.0, max(0.0, (x + 0.2) / 0.4)) if half > 0.2 else (1.0 if x > 0.0 else 0.0)
            t = t * t * (3.0 - 2.0 * t)
            joints.append((0, 1, 0, 0))
            weights.append((1.0 - t, t, 0.0, 0.0))

    indices: list[int] = []
    for row in range(grid):
        for col in range(grid):
            a = row * (grid + 1) + col
            b = a + 1
            c = a + (grid + 1)
            d = c + 1
            indices.extend((a, b, c, b, d, c))

    # ---- the two joints' inverse bind matrices ----
    # Both joints sit at the pole (the plane's left edge) in the BIND pose, so both bind worlds are
    # translate(-half, 0, 0) and both inverse binds are translate(+half, 0, 0). The animated joint is
    # therefore the pivot the flag swings around, and the mesh node itself stays at the origin so the
    # controller's mesh_world_inv term is the identity.
    ibm_translation = half
    inverse_bind = [
        1.0, 0.0, 0.0, 0.0,
        0.0, 1.0, 0.0, 0.0,
        0.0, 0.0, 1.0, 0.0,
        ibm_translation, 0.0, 0.0, 1.0,
    ] * 2

    # ---- the animation: a rotation about Z on the animated joint ----
    times = list(KEY_TIMES)
    rotations: list[float] = []
    for angle in KEY_ANGLES_DEG:
        radians = math.radians(angle)
        rotations.extend((0.0, 0.0, math.sin(radians * 0.5), math.cos(radians * 0.5)))  # [x, y, z, w]

    buffer = Buffer()
    view_position = buffer.add(struct.pack(f"<{len(positions) * 3}f", *(c for p in positions for c in p)))
    view_normal = buffer.add(struct.pack(f"<{len(normals) * 3}f", *(c for n in normals for c in n)))
    view_joints = buffer.add(struct.pack(f"<{len(joints) * 4}B", *(c for j in joints for c in j)))
    view_weights = buffer.add(struct.pack(f"<{len(weights) * 4}f", *(c for w in weights for c in w)))
    view_indices = buffer.add(struct.pack(f"<{len(indices)}H", *indices), stride_align=2)
    view_ibm = buffer.add(struct.pack(f"<{len(inverse_bind)}f", *inverse_bind))
    view_times = buffer.add(struct.pack(f"<{len(times)}f", *times))
    view_rotations = buffer.add(struct.pack(f"<{len(rotations)}f", *rotations))

    def view(index: int) -> dict:
        offset, length = buffer.views[index]
        return {"buffer": 0, "byteOffset": offset, "byteLength": length}

    document = {
        "asset": {
            "version": "2.0",
            "generator": "scripts/make_animated_skin_fixture.py: one skin, two joints, a swinging flag",
        },
        "scene": 0,
        "scenes": [{"nodes": [0, 2]}],
        # 0 root, 1 pole (static joint), 2 the mesh node (STATIC - so the rigid motion term is zero),
        # 3 arm (the animated joint, a child of the pole)
        "nodes": [
            {"name": "root", "children": [1]},
            {"name": "pole", "translation": [-half, 0.0, 0.0], "children": [3]},
            {"name": "flag", "mesh": 0, "skin": 0},
            {"name": "arm"},
        ],
        "skins": [{"name": "flag_skin", "joints": [1, 3], "skeleton": 1, "inverseBindMatrices": 5}],
        "meshes": [
            {
                "name": "flag",
                "primitives": [
                    {
                        "attributes": {"POSITION": 0, "NORMAL": 1, "JOINTS_0": 2, "WEIGHTS_0": 3},
                        "indices": 4,
                        "material": 0,
                    }
                ],
            }
        ],
        "materials": [
            {
                "name": "flag",
                # matte and neutral: a specular highlight would move with the light rather than with
                # the surface and make the motion-channel reading harder to interpret
                "pbrMetallicRoughness": {"baseColorFactor": [0.75, 0.75, 0.78, 1.0], "metallicFactor": 0.0, "roughnessFactor": 1.0},
                "doubleSided": True,
            }
        ],
        "animations": [
            {
                "name": "swing",
                "samplers": [{"input": 6, "output": 7, "interpolation": "LINEAR"}],
                "channels": [{"sampler": 0, "target": {"node": 3, "path": "rotation"}}],
            }
        ],
        "accessors": [
            {
                "bufferView": view_position,
                "componentType": FLOAT,
                "count": len(positions),
                "type": "VEC3",
                "min": [-half, -half, 0.0],
                "max": [half, half, 0.0],
            },
            {"bufferView": view_normal, "componentType": FLOAT, "count": len(normals), "type": "VEC3"},
            {"bufferView": view_joints, "componentType": UNSIGNED_BYTE, "count": len(joints), "type": "VEC4"},
            {"bufferView": view_weights, "componentType": FLOAT, "count": len(weights), "type": "VEC4"},
            {"bufferView": view_indices, "componentType": UNSIGNED_SHORT, "count": len(indices), "type": "SCALAR"},
            {"bufferView": view_ibm, "componentType": FLOAT, "count": 2, "type": "MAT4"},
            {"bufferView": view_times, "componentType": FLOAT, "count": len(times), "type": "SCALAR", "min": [times[0]], "max": [times[-1]]},
            {"bufferView": view_rotations, "componentType": FLOAT, "count": len(times), "type": "VEC4"},
        ],
        "bufferViews": [view(i) for i in range(len(buffer.views))],
        "buffers": [
            {
                "byteLength": len(buffer.data),
                "uri": "data:application/octet-stream;base64," + base64.b64encode(bytes(buffer.data)).decode("ascii"),
            }
        ],
    }
    return document


def main() -> int:
    args = parse_args()
    document = build(args)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
    vertices = document["accessors"][0]["count"]
    triangles = document["accessors"][4]["count"] // 3
    byte_length = document["buffers"][0]["byteLength"]
    print(f"wrote {args.out}")
    print(f"  grid {args.grid}x{args.grid}: {vertices} vertices, {triangles} triangles, {byte_length} bytes of geometry")
    print(f"  skin: 2 joints (pole pinned, arm animated about Z), weights blended across x")
    print(f"  animation 'swing': {len(KEY_TIMES)} keyframes over {KEY_TIMES[-1]} s, angles {KEY_ANGLES_DEG} deg")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
