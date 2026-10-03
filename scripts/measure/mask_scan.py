"""Which sample assets would the mask bake actually change?

For every alphaMode MASK material in the sample assets, sample the base-colour texture's ALPHA at
each triangle's three vertex UVs, apply the material's cutoff, and count the triangles that are cut
at all three samples ("collapsed") against those that survive somewhere ("kept").

That is the rule a per-triangle bake can implement, so this is the reconnaissance that decides
whether the feature is measurable on an asset that already exists - and it is the number to check
the implementation against.

Usage: python mask_scan.py <ModelsDir> [asset ...]
"""
import glob
import json
import os
import struct
import sys

from PIL import Image

MODELS = sys.argv[1]
WANTED = sys.argv[2:]


def load_buffer(gltf_dir, buf):
    uri = buf.get("uri")
    if uri is None:
        return None  # GLB: not handled here
    path = os.path.join(gltf_dir, uri.replace("%20", " "))
    with open(path, "rb") as f:
        return f.read()


def read_accessor(gltf, gltf_dir, buffers, index):
    acc = gltf["accessors"][index]
    view = gltf["bufferViews"][acc["bufferView"]]
    data = buffers[view.get("buffer", 0)]
    comp = acc["componentType"]
    n = acc["count"]
    ncomp = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}[acc["type"]]
    fmt = {5121: "B", 5123: "H", 5125: "I", 5126: "f"}[comp]
    size = struct.calcsize(fmt)
    stride = view.get("byteStride") or size * ncomp
    base = view.get("byteOffset", 0) + acc.get("byteOffset", 0)
    out = []
    for i in range(n):
        off = base + i * stride
        out.append(struct.unpack_from("<" + fmt * ncomp, data, off))
    return out


def texture_alpha(gltf, gltf_dir, tex_index):
    tex = gltf["textures"][tex_index]
    img = gltf["images"][tex["source"]]
    path = os.path.join(gltf_dir, img["uri"].replace("%20", " "))
    im = Image.open(path).convert("RGBA")
    return im


def uv_to_px(uv, size, flip_v):
    x = int(uv[0] % 1.0 * size[0])
    y = int(uv[1] % 1.0 * size[1])
    if flip_v:
        y = size[1] - 1 - y
    return max(0, min(size[0] - 1, x)), max(0, min(size[1] - 1, y))


FLIP_V = False  # the engine's textures are uploaded with row 0 = the image's top row, so uv.y = 0 is
                # the FIRST row: no flip. (The first version of this scan flipped it, which is why its
                # prediction did not match what the shader did - the mask was evaluated mirrored.)


for path in sorted(glob.glob(os.path.join(MODELS, "*", "glTF", "*.gltf"))):
    asset = os.path.basename(os.path.dirname(os.path.dirname(path)))
    if WANTED and asset not in WANTED:
        continue
    gltf = json.load(open(path))
    gltf_dir = os.path.dirname(path)
    buffers = {}
    ok = True
    for i, b in enumerate(gltf.get("buffers", [])):
        data = load_buffer(gltf_dir, b)
        if data is None:
            ok = False
            break
        buffers[i] = data
    if not ok:
        continue
    masks = [(i, m) for i, m in enumerate(gltf.get("materials", [])) if m.get("alphaMode") == "MASK"]
    if not masks:
        continue
    for mat_index, mat in masks:
        pbr = mat.get("pbrMetallicRoughness", {})
        if "baseColorTexture" not in pbr:
            continue
        cutoff = mat.get("alphaCutoff", 0.5)
        try:
            im = texture_alpha(gltf, gltf_dir, pbr["baseColorTexture"]["index"])
        except Exception as e:
            print(f"{asset}: material {mat_index}: texture failed: {e}")
            continue
        px = im.load()
        size = im.size
        total = vert_cut = area_cut = 0
        for mesh in gltf.get("meshes", []):
            for prim in mesh.get("primitives", []):
                if prim.get("material") != mat_index:
                    continue
                if "TEXCOORD_0" not in prim["attributes"] or "indices" not in prim:
                    continue
                uvs = read_accessor(gltf, gltf_dir, buffers, prim["attributes"]["TEXCOORD_0"])
                idx = [i[0] for i in read_accessor(gltf, gltf_dir, buffers, prim["indices"])]
                # Two rules, because the difference between them IS the feature's honest limit:
                #  - "vertex": collapse when the three VERTEX UVs are all cut. Cheap, and it destroys any
                #    triangle whose opaque content lives strictly inside it (a quad with a pattern in the
                #    middle samples transparent at all four corners).
                #  - "area": collapse only when a 4x4 barycentric grid over the triangle is ENTIRELY cut,
                #    i.e. when the triangle's whole area is transparent. That is the rule a bake can defend.
                grid = []
                for i in range(4):
                    for j in range(4 - i):
                        a = (i + 1.0 / 3.0) / 4.0
                        b = (j + 1.0 / 3.0) / 4.0
                        grid.append((a, b, 1.0 - a - b))
                for t in range(0, len(idx) - 2, 3):
                    corners = [uvs[idx[t + k]][:2] for k in range(3)]

                    def opaque(uv):
                        return px[uv_to_px(uv, size)][3] / 255.0 >= cutoff

                    if not any(opaque(uv) for uv in corners):
                        vert_cut += 1
                    cut_area = True
                    for (w0, w1, w2) in grid:
                        u = corners[0][0] * w0 + corners[1][0] * w1 + corners[2][0] * w2
                        v = corners[0][1] * w0 + corners[1][1] * w1 + corners[2][1] * w2
                        if opaque((u, v)):
                            cut_area = False
                            break
                    if cut_area:
                        area_cut += 1
                    total += 1
        print(f"{asset:28s} mat {mat_index:3d} cutoff {cutoff:<5} tri {total:6d}  "
              f"all-3-vertices-cut {100.0*vert_cut/max(total,1):5.1f}%   whole-area-cut {100.0*area_cut/max(total,1):5.1f}%")
