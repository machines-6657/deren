"""How aggressive is a per-triangle mask rule, under three different evaluations?

Usage: python mask_rules.py <gltf> [materialIndex]

Reports, for one glTF's MASK material, the share of triangles a per-triangle bake would COLLAPSE
(remove from the acceleration structure) under:
  vertices   exactly the 3 vertex UVs (the cheapest rule, and the one that eats visible geometry)
  10-sample  a 4x4 triangular barycentric grid over the area (what shaders/mask_bake.comp does)
  dense      every texel of the triangle's UV bounding box that lies inside the UV triangle - the
             closest thing to "the whole area is transparent" that a CPU check can produce
and the share the RASTER path effectively keeps, which is what the bake has to agree with: a
triangle matters only if some fragment of it survives the per-pixel discard.

The engine's UV convention is uv.y = 0 at the texture's FIRST row (the images are uploaded with
row 0 at the top), so no V flip is applied here.
"""
import json
import os
import struct
import sys

from PIL import Image

path = sys.argv[1]
only_material = int(sys.argv[2]) if len(sys.argv) > 2 else None
gltf_dir = os.path.dirname(path)
gltf = json.load(open(path))


def buffer_bytes():
    out = {}
    for i, b in enumerate(gltf.get("buffers", [])):
        with open(os.path.join(gltf_dir, b["uri"].replace("%20", " ")), "rb") as f:
            out[i] = f.read()
    return out


def accessor(buffers, index):
    acc = gltf["accessors"][index]
    view = gltf["bufferViews"][acc["bufferView"]]
    data = buffers[view.get("buffer", 0)]
    ncomp = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}[acc["type"]]
    fmt = {5121: "B", 5123: "H", 5125: "I", 5126: "f"}[acc["componentType"]]
    size = struct.calcsize(fmt)
    stride = view.get("byteStride") or size * ncomp
    base = view.get("byteOffset", 0) + acc.get("byteOffset", 0)
    return [struct.unpack_from("<" + fmt * ncomp, data, base + i * stride) for i in range(acc["count"])]


def texel(px, uv, size):
    x = max(0, min(size[0] - 1, int(uv[0] % 1.0 * size[0])))
    y = max(0, min(size[1] - 1, int(uv[1] % 1.0 * size[1])))
    return px[x, y][3] / 255.0


buffers = buffer_bytes()
grid = []
for i in range(4):
    for j in range(4 - i):
        a = (i + 1.0 / 3.0) / 4.0
        b = (j + 1.0 / 3.0) / 4.0
        grid.append((a, b, 1.0 - a - b))

for mat_index, material in enumerate(gltf.get("materials", [])):
    if material.get("alphaMode") != "MASK" or (only_material is not None and mat_index != only_material):
        continue
    pbr = material.get("pbrMetallicRoughness", {})
    cutoff = material.get("alphaCutoff", 0.5)
    image = gltf["images"][gltf["textures"][pbr["baseColorTexture"]["index"]]["source"]]
    im = Image.open(os.path.join(gltf_dir, image["uri"].replace("%20", " "))).convert("RGBA")
    px, size = im.load(), im.size

    def opaque(uv):
        return texel(px, uv, size) >= cutoff

    total = vertex_cut = grid_cut = dense_cut = 0
    for mesh in gltf.get("meshes", []):
        for prim in mesh.get("primitives", []):
            if prim.get("material") != mat_index or "TEXCOORD_0" not in prim["attributes"] or "indices" not in prim:
                continue
            uvs = accessor(buffers, prim["attributes"]["TEXCOORD_0"])
            idx = [i[0] for i in accessor(buffers, prim["indices"])]
            for t in range(0, len(idx) - 2, 3):
                corners = [uvs[idx[t + k]][:2] for k in range(3)]
                total += 1
                if not any(opaque(uv) for uv in corners):
                    vertex_cut += 1
                if not any(opaque((corners[0][0] * w0 + corners[1][0] * w1 + corners[2][0] * w2,
                                   corners[0][1] * w0 + corners[1][1] * w1 + corners[2][1] * w2)) for (w0, w1, w2) in grid):
                    grid_cut += 1
                us = [c[0] for c in corners]
                vs = [c[1] for c in corners]
                steps_u = min(48, max(2, int(abs(max(us) - min(us)) * size[0]) + 1))
                steps_v = min(48, max(2, int(abs(max(vs) - min(vs)) * size[1]) + 1))
                dense = True
                for iu in range(steps_u):
                    if not dense:
                        break
                    for iv in range(steps_v):
                        u = min(us) + (iu + 0.5) / steps_u * (max(us) - min(us))
                        v = min(vs) + (iv + 0.5) / steps_v * (max(vs) - min(vs))
                        w0 = (corners[1][1] - corners[2][1]) * (u - corners[2][0]) + (corners[2][0] - corners[1][0]) * (v - corners[2][1])
                        w1 = (corners[2][1] - corners[0][1]) * (u - corners[2][0]) + (corners[0][0] - corners[2][0]) * (v - corners[2][1])
                        w2 = 1.0 - w0 - w1
                        if min(w0, w1, w2) < -0.02:
                            continue
                        if opaque((u, v)):
                            dense = False
                            break
                if dense:
                    dense_cut += 1
    print(f"{os.path.basename(os.path.dirname(gltf_dir))} material {mat_index} cutoff {cutoff}: {total} triangles")
    print(f"    vertices {100.0*vertex_cut/max(total,1):5.1f}% fully cut   10-sample {100.0*grid_cut/max(total,1):5.1f}%   dense {100.0*dense_cut/max(total,1):5.1f}%")
