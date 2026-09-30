"""Procedurally build Patina's test assets and export them as .glb.

Run (from the repo root):
    /Applications/Blender.app/Contents/MacOS/Blender -b --factory-startup \
        --python examples/make_assets.py -- --out examples/assets

Options (after `--`):
    --out DIR                 output directory (default: examples/assets next to this script)
    --only crate,barrel,...   build a subset
    --uv-mode per-material    (default) every material / texture set gets its own full 0-1 UV
                              space (Substance-style texture sets; islands of *different*
                              materials may overlap each other, never within one material)
    --uv-mode shared          one packed 0-1 layout for the whole object, no overlaps at all
    --no-check                skip the UV rasterisation overlap check (and the angle fallback)

Assets (all +Y up in glTF, modifiers applied, split/custom normals, TEXCOORD_0, materials):
    crate.glb    beveled paneled box, convex + concave edges       material: Crate
    barrel.glb   bulged barrel with raised bands and inset lids     materials: BarrelBody, BarrelRings
    hammer.glb   two objects: Handle + Head                         materials: Wood, Steel
    suzanne.glb  Suzanne, subdivision level 2 applied, smooth       material: Skin
    panel.glb    sci-fi panel with recessed grooves + bolts         material: Panel

UVs:
  * hard-surface / organic meshes: Smart UV Project (angle 66, island margin 0.02) is used to
    segment islands, the island borders become seams, islands are relaxed with the
    Minimum-Stretch unwrapper (removes the projection stretch of faces seen at an angle) and
    then packed (rotation on, margin 0.006 of UV space). The result is rasterised; if any
    texel is covered twice inside one material the whole thing is redone with a lower angle
    limit (66 -> 55 -> 45 -> 35).
  * lathe meshes (barrel, handle) get analytic cylindrical UVs (arc length x profile length,
    planar lids), which pack far better than Smart UV's thin annuli, then are packed the same way.

The script exits with a nonzero status if anything fails (Blender itself would exit 0 on an
uncaught Python exception).
"""

import argparse
import json
import math
import os
import struct
import sys
import traceback

import bpy
import bmesh
from mathutils import Matrix

ASSET_NAMES = ("crate", "barrel", "hammer", "suzanne", "panel")

UV_ANGLES = (66.0, 55.0, 45.0, 35.0)   # smart-project angle limits tried in order
UV_ISLAND_MARGIN = 0.02                # smart project margin
UV_PACK_MARGIN = 0.006                 # pack margin, fraction of UV space (~6px @1k, 12px @2k)

OPTS = {"uv_mode": "per-material", "check": True}   # set from the command line in main()


# --------------------------------------------------------------------------------------
# generic helpers
# --------------------------------------------------------------------------------------

def parse_args():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    here = os.path.dirname(os.path.abspath(__file__))
    p = argparse.ArgumentParser(prog="make_assets.py",
                                description="Build Patina test assets (.glb) procedurally.")
    p.add_argument("--out", default=os.path.join(here, "assets"))
    p.add_argument("--only", default="", help="comma separated subset of: " + ",".join(ASSET_NAMES))
    p.add_argument("--uv-mode", choices=("per-material", "shared"), default="per-material")
    p.add_argument("--no-check", action="store_true", help="skip UV overlap rasterisation check")
    return p.parse_args(argv)


def reset_scene():
    for o in list(bpy.data.objects):
        bpy.data.objects.remove(o, do_unlink=True)
    for coll in (bpy.data.meshes, bpy.data.materials, bpy.data.cameras, bpy.data.lights,
                 bpy.data.images, bpy.data.curves):
        for d in list(coll):
            coll.remove(d)
    bpy.context.view_layer.update()


def make_material(name, color, metallic=0.0, roughness=0.5):
    m = bpy.data.materials.new(name)
    if m.node_tree is None:          # pre-5.0 behaviour
        m.use_nodes = True
    bsdf = next(n for n in m.node_tree.nodes if n.type == "BSDF_PRINCIPLED")
    bsdf.inputs["Base Color"].default_value = (*color, 1.0)
    bsdf.inputs["Metallic"].default_value = metallic
    bsdf.inputs["Roughness"].default_value = roughness
    m.diffuse_color = (*color, 1.0)
    m.metallic = metallic
    m.roughness = roughness
    return m


def object_from_bmesh(name, bm, materials):
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces[:])
    me = bpy.data.meshes.new(name)
    bm.to_mesh(me)
    bm.free()
    for m in materials:
        me.materials.append(m)
    obj = bpy.data.objects.new(name, me)
    bpy.context.scene.collection.objects.link(obj)
    return obj


def add_bevel(obj, width, segments, angle_deg=30.0):
    obj.data.shade_smooth()          # harden_normals needs smooth faces
    mod = obj.modifiers.new("Bevel", "BEVEL")
    mod.width = width
    mod.segments = segments
    mod.limit_method = "ANGLE"
    mod.angle_limit = math.radians(angle_deg)
    mod.use_clamp_overlap = True
    mod.harden_normals = True        # flat faces stay flat, bevels get smooth custom normals
    return mod


def apply_modifiers(obj):
    """Bake the modifier stack into the mesh (context free, keeps custom normals + materials)."""
    if not obj.modifiers:
        return
    dg = bpy.context.evaluated_depsgraph_get()
    new_me = bpy.data.meshes.new_from_object(obj.evaluated_get(dg), preserve_all_data_layers=True,
                                             depsgraph=dg)
    old = obj.data
    obj.modifiers.clear()
    obj.data = new_me
    bpy.data.meshes.remove(old)
    new_me.name = obj.name


def smooth_by_angle(obj, angle_deg=30.0):
    me = obj.data
    me.shade_smooth()
    me.set_sharp_from_angle(angle=math.radians(angle_deg))


def select_only(objs):
    bpy.context.view_layer.update()
    for o in bpy.context.scene.objects:
        o.select_set(False)
    for o in objs:
        o.select_set(True)
    bpy.context.view_layer.objects.active = objs[0]


def _select_faces(bm, me, mi):
    """Select exactly the faces of material index mi (None = all) in face-select mode."""
    bm.select_mode = {"FACE"}
    for f in bm.faces:
        f.select_set(False)
    for f in bm.faces:
        if mi is None or f.material_index == mi:
            f.select_set(True)
    bm.select_flush_mode()
    bmesh.update_edit_mesh(me)


def _pack_selected():
    bpy.ops.uv.select_all(action="SELECT")
    bpy.ops.uv.pack_islands(rotate=True, rotate_method="ANY", scale=True,
                            margin_method="FRACTION", margin=UV_PACK_MARGIN,
                            shape_method="CONCAVE")


def unwrap(obj, uv_mode=None, check=None):
    """Unwrap + pack. per-material: each material index gets its own full 0-1 space.
    Returns the per-material uv report of the accepted layout. Objects flagged
    'patina_uv_done' (unwrapped by their builder before beveling) are only re-checked."""
    uv_mode = OPTS["uv_mode"] if uv_mode is None else uv_mode
    check = OPTS["check"] if check is None else check
    if obj.get("patina_uv_done"):
        report = uv_report(obj, check)
        report["_method"] = obj["patina_uv_done"]
        return report
    me = obj.data
    if not me.uv_layers:
        me.uv_layers.new(name="UVMap")
    analytic = bool(obj.get("patina_analytic_uv"))     # UVs already written by the builder
    seamed = bool(obj.get("patina_seamed_uv"))         # builder marked seams: plain unwrap
    groups = (sorted({p.material_index for p in me.polygons}) if uv_mode == "per-material"
              else [None])
    orig_seams = [False] * len(me.edges)
    me.edges.foreach_get("use_seam", orig_seams)
    select_only([obj])
    # Blender 5.x defaults to UV sync selection, where uv.select_all() selects the whole mesh
    # and pack_islands() would pack every material together. Turn it off so the UV ops only
    # see the faces selected for the current material.
    bpy.context.scene.tool_settings.use_uv_select_sync = False

    angles = [None] if (analytic or seamed) else (UV_ANGLES if check else UV_ANGLES[:1])
    report = None
    for angle in angles:
        me.edges.foreach_set("use_seam", orig_seams)
        bpy.ops.object.mode_set(mode="EDIT")
        try:
            bm = bmesh.from_edit_mesh(me)
            for mi in groups:
                _select_faces(bm, me, mi)
                if seamed:
                    bpy.ops.uv.select_all(action="SELECT")
                    bpy.ops.uv.unwrap(method="ANGLE_BASED", fill_holes=True, correct_aspect=True,
                                      margin_method="FRACTION", margin=UV_PACK_MARGIN)
                elif not analytic:
                    bpy.ops.uv.smart_project(angle_limit=math.radians(angle),
                                             island_margin=UV_ISLAND_MARGIN, area_weight=0.0,
                                             correct_aspect=True, scale_to_bounds=False)
                    bpy.ops.uv.select_all(action="SELECT")
                    bpy.ops.uv.seams_from_islands(mark_seams=True, mark_sharp=False)
                    for e in bm.edges:                 # keep the authored seams as well
                        if orig_seams[e.index]:
                            e.seam = True
                    bmesh.update_edit_mesh(me)
                    bpy.ops.uv.unwrap(method="MINIMUM_STRETCH", fill_holes=True,
                                      correct_aspect=True, margin_method="FRACTION",
                                      margin=UV_PACK_MARGIN, iterations=10)
                _pack_selected()
            _select_faces(bm, me, -1)          # deselect everything
        finally:
            bpy.ops.object.mode_set(mode="OBJECT")
        report = uv_report(obj, check)
        report["_method"] = ("analytic" if analytic else "seams+abf" if seamed
                             else "smart%d+min_stretch" % angle)
        if not check or all(r.get("overlap_px", 0) == 0 for k, r in report.items()
                            if not k.startswith("_")):
            break
        print("  %s: UV overlap at angle %s, retrying with a lower angle" % (obj.name, angle))
    return report


def uv_report(obj, do_raster, res=512):
    """Per material: UV bounds, coverage and (optionally) rasterised overlap fraction."""
    import numpy as np
    me = obj.data
    me.calc_loop_triangles()
    uv = me.uv_layers.active.data
    per_mat, area3d = {}, {}
    for lt in me.loop_triangles:
        per_mat.setdefault(lt.material_index, []).append([tuple(uv[li].uv) for li in lt.loops])
        area3d.setdefault(lt.material_index, []).append(lt.area)
    out = {}
    for mi, tris in sorted(per_mat.items()):
        t = np.asarray(tris, dtype=np.float64)            # (n,3,2)
        lo, hi = t.reshape(-1, 2).min(0), t.reshape(-1, 2).max(0)
        e1, e2 = t[:, 1] - t[:, 0], t[:, 2] - t[:, 0]
        uva = 0.5 * np.abs(e1[:, 0] * e2[:, 1] - e1[:, 1] * e2[:, 0])
        a3 = np.asarray(area3d[mi])
        area = uva.sum()
        # texel density uniformity: sqrt(uv area / 3d area) relative to the material average,
        # 5th..95th percentile weighted by surface area (1.0 = perfectly uniform)
        ok = a3 > 1e-12
        dens = np.sqrt((uva[ok] / a3[ok]) / (area / a3[ok].sum()))
        order = np.argsort(dens)
        cw = np.cumsum(a3[ok][order]) / a3[ok].sum()
        p5, p95 = dens[order][np.searchsorted(cw, 0.05)], dens[order][min(np.searchsorted(cw, 0.95), len(cw) - 1)]
        name = me.materials[mi].name if mi < len(me.materials) else str(mi)
        rep = {"tris": len(tris), "uv_min": [round(float(x), 4) for x in lo],
               "uv_max": [round(float(x), 4) for x in hi], "coverage": round(float(area), 3),
               "texel_density_p5_p95": [round(float(p5), 2), round(float(p95), 2)]}
        if do_raster:
            cnt = np.zeros((res, res), np.uint8)
            p = t * res - 0.5                              # pixel-centre space
            eps = 1e-4
            for a, b, c in p:
                x0 = max(int(math.floor(min(a[0], b[0], c[0]))), 0)
                x1 = min(int(math.ceil(max(a[0], b[0], c[0]))), res - 1)
                y0 = max(int(math.floor(min(a[1], b[1], c[1]))), 0)
                y1 = min(int(math.ceil(max(a[1], b[1], c[1]))), res - 1)
                if x1 < x0 or y1 < y0:
                    continue
                d = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])
                if abs(d) < 1e-12:
                    continue
                xs, ys = np.meshgrid(np.arange(x0, x1 + 1), np.arange(y0, y1 + 1))
                w0 = ((b[0] - xs) * (c[1] - ys) - (b[1] - ys) * (c[0] - xs)) / d
                w1 = ((c[0] - xs) * (a[1] - ys) - (c[1] - ys) * (a[0] - xs)) / d
                w2 = 1.0 - w0 - w1
                inside = (w0 > eps) & (w1 > eps) & (w2 > eps)
                cnt[y0:y1 + 1, x0:x1 + 1] += inside.astype(np.uint8)
            covered = int((cnt > 0).sum())
            rep["overlap_px"] = int((cnt > 1).sum())
            rep["overlap_frac"] = round(rep["overlap_px"] / max(covered, 1), 5)
        out[name] = rep
    return out


def glb_info(path):
    """Parse the GLB JSON chunk: triangle count, material names, primitive attributes."""
    with open(path, "rb") as f:
        data = f.read()
    magic, _ver, _length = struct.unpack_from("<4sII", data, 0)
    if magic != b"glTF":
        raise RuntimeError("%s is not a GLB file" % path)
    clen, ctype = struct.unpack_from("<II", data, 12)
    if ctype != 0x4E4F534A:
        raise RuntimeError("%s: first chunk is not JSON" % path)
    g = json.loads(data[20:20 + clen])
    tris, attrs = 0, set()
    for mesh in g.get("meshes", []):
        for prim in mesh["primitives"]:
            attrs.update(prim["attributes"].keys())
            if "indices" in prim:
                tris += g["accessors"][prim["indices"]]["count"] // 3
            else:
                tris += g["accessors"][prim["attributes"]["POSITION"]]["count"] // 3
    return {"triangles": tris, "materials": [m.get("name", "?") for m in g.get("materials", [])],
            "nodes": [n.get("name", "?") for n in g.get("nodes", []) if "mesh" in n],
            "attributes": sorted(attrs), "bytes": len(data)}


def export_glb(path):
    bpy.ops.export_scene.gltf(
        filepath=path, export_format="GLB", use_selection=False, export_apply=True,
        export_yup=True, export_normals=True, export_texcoords=True, export_tangents=False,
        export_materials="EXPORT", export_cameras=False, export_lights=False,
        export_animations=False, export_extras=False, export_image_format="NONE")


# --------------------------------------------------------------------------------------
# lathe helper (barrel, hammer handle) with analytic UVs
# --------------------------------------------------------------------------------------

def revolve(bm, profile, segments, u_splits=1, turn_split_deg=45.0, merge_len=0.03,
            max_len=None):
    """profile: list of (radius, z, material_index of the segment to the next point); radius 0
    makes a pole (triangle fan). Also writes analytic UVs (world-unit scale, packed later):
      - side strips: u = arc length around the axis, v = length along the profile;
        a new island starts on a material change or a turn > turn_split_deg between two
        segments that are both >= merge_len long (short steps stay attached);
      - pole fans (lids): planar XY projection, one island each;
      - the circumference is cut into u_splits islands and strips longer than max_len are cut
        at ring boundaries, to keep islands compact (better packing)."""
    uvl = bm.loops.layers.uv.get("UVMap") or bm.loops.layers.uv.new("UVMap")
    rings = []
    for r, z, _mi in profile:
        if r <= 1e-9:
            rings.append([bm.verts.new((0.0, 0.0, z))])
        else:
            rings.append([bm.verts.new((r * math.cos(2 * math.pi * i / segments),
                                        r * math.sin(2 * math.pi * i / segments), z))
                          for i in range(segments)])
    nseg = len(profile) - 1
    seglen, dirs = [], []
    for k in range(nseg):
        dr, dz = profile[k + 1][0] - profile[k][0], profile[k + 1][1] - profile[k][1]
        ln = math.hypot(dr, dz)
        seglen.append(ln)
        dirs.append((dr / ln, dz / ln))

    def is_fan(k):
        return len(rings[k]) == 1 or len(rings[k + 1]) == 1

    v_at = {}        # (segment k, ring index kk) -> v
    v = 0.0
    for k in range(nseg):
        new = k == 0 or is_fan(k) or is_fan(k - 1) or profile[k][2] != profile[k - 1][2]
        if not new:
            cosang = dirs[k][0] * dirs[k - 1][0] + dirs[k][1] * dirs[k - 1][1]
            turn = math.degrees(math.acos(max(-1.0, min(1.0, cosang))))
            new = turn > turn_split_deg and seglen[k] >= merge_len and seglen[k - 1] >= merge_len
        if not new and max_len and v > 0.0 and v + seglen[k] > max_len:
            new = True
        if new:
            v = 0.0
        v_at[(k, k)] = v
        v += seglen[k]
        v_at[(k, k + 1)] = v

    sps = segments // u_splits
    for k in range(nseg):
        a, b, mi = rings[k], rings[k + 1], profile[k][2]
        for i in range(segments):
            j = (i + 1) % segments
            if len(a) == 1:
                f = bm.faces.new((a[0], b[i], b[j]))
            elif len(b) == 1:
                f = bm.faces.new((a[i], b[0], a[j]))
            else:
                f = bm.faces.new((a[i], b[i], b[j], a[j]))
            f.material_index = mi
            if is_fan(k):
                for loop in f.loops:
                    loop[uvl].uv = (loop.vert.co.x, loop.vert.co.y)
                continue
            s = i // sps
            for loop in f.loops:
                kk = k if loop.vert in a else k + 1
                ring = rings[kk]
                ii = ring.index(loop.vert) if len(ring) > 1 else 0
                if ii == 0 and i == segments - 1:
                    ii = segments
                r = profile[kk][0]
                loop[uvl].uv = ((ii - s * sps) / segments * 2 * math.pi * r, v_at[(k, kk)])
    return rings


# --------------------------------------------------------------------------------------
# assets
# --------------------------------------------------------------------------------------

def build_crate():
    mat = make_material("Crate", (0.45, 0.30, 0.16), 0.0, 0.7)
    RIM, RECESS, FRAME, PLATE = 0.075, 0.035, 0.06, 0.02
    bm = bmesh.new()
    bmesh.ops.create_cube(bm, size=1.0)
    faces = bm.faces[:]
    # outer frame rim, then a recessed panel (concave edges) holding a raised plate (convex)
    bmesh.ops.inset_individual(bm, faces=faces, thickness=RIM, use_even_offset=True)
    bmesh.ops.inset_individual(bm, faces=faces, thickness=0.0, depth=-RECESS)
    bmesh.ops.inset_individual(bm, faces=faces, thickness=FRAME, use_even_offset=True)
    bmesh.ops.inset_individual(bm, faces=faces, thickness=0.0, depth=PLATE)
    obj = object_from_bmesh("Crate", bm, [mat])
    obj.location.z = 0.5                       # sits on the ground
    add_bevel(obj, width=0.006, segments=3)
    apply_modifiers(obj)

    # Seams (marked on the beveled mesh, so the bevels are unwrapped too): between the six
    # sides, and along the diagonals of each side except across the raised plate's top.
    # Every side then unfolds (almost distortion free, it is developable apart from the
    # small bevel corner patches) into one cross-shaped island: plate + 4 arms of
    # plate wall / floor / recess wall / rim. Hollow frame islands would waste UV space.
    plate_h = 0.5 - RECESS + PLATE

    def classify(f):
        c = f.calc_center_median()
        ax = max(range(3), key=lambda i: abs(c[i]))
        sgn = 1 if c[ax] >= 0 else -1
        u, v = [c[i] for i in range(3) if i != ax]
        is_plate = f.normal[ax] * sgn > 0.999 and abs(abs(c[ax]) - plate_h) < 1e-4
        sector = "plate" if is_plate else ((0, u >= 0) if abs(u) >= abs(v) else (1, v >= 0))
        return (ax, sgn), sector

    bm = bmesh.new()
    bm.from_mesh(obj.data)
    cls = {f.index: classify(f) for f in bm.faces}
    for e in bm.edges:
        if len(e.link_faces) != 2:
            continue
        (s1, k1), (s2, k2) = cls[e.link_faces[0].index], cls[e.link_faces[1].index]
        e.seam = s1 != s2 or (k1 != k2 and "plate" not in (k1, k2))
    bm.to_mesh(obj.data)
    bm.free()
    obj["patina_seamed_uv"] = True
    return [obj]


def build_barrel():
    body = make_material("BarrelBody", (0.42, 0.26, 0.13), 0.0, 0.65)
    rings = make_material("BarrelRings", (0.35, 0.35, 0.37), 1.0, 0.45)
    H, R_END, R_MID, BAND, RIM, LID = 1.2, 0.30, 0.345, 0.012, 0.028, 0.02
    bands = [(0.09, 0.16), (0.36, 0.42), (0.78, 0.84), (1.04, 1.11)]

    def r_at(z):
        t = (z - H / 2) / (H / 2)
        return R_END + (R_MID - R_END) * (1.0 - t * t)

    # z samples along the side: regular spacing + band boundaries/midpoints
    zs = {round(i * H / 30, 5) for i in range(31)}
    for z0, z1 in bands:
        zs = {z for z in zs if not (z0 - 0.01 < z < z1 + 0.01)}
        zs.update({z0, z1, round((z0 + z1) / 2, 5)})
    zs = sorted(zs)

    def band_of(z):
        for z0, z1 in bands:
            if z0 - 1e-6 <= z <= z1 + 1e-6:
                return (z0, z1)
        return None

    prof = [(0.0, LID, 0), (r_at(0) - RIM, LID, 0), (r_at(0) - RIM, 0.0, 0)]  # bottom lid + rim
    for z in zs:
        b = band_of(z)
        if b and abs(z - b[0]) < 1e-6:            # step out onto the band
            prof.append((r_at(z), z, 1))
            prof.append((r_at(z) + BAND, z, 1))
        elif b and abs(z - b[1]) < 1e-6:          # step back down to the body
            prof.append((r_at(z) + BAND, z, 1))
            prof.append((r_at(z), z, 0))
        elif b:
            prof.append((r_at(z) + BAND, z, 1))
        else:
            prof.append((r_at(z), z, 0))
    prof += [(r_at(H) - RIM, H, 0), (r_at(H) - RIM, H - LID, 0), (0.0, H - LID, 0)]  # top rim + lid
    bm = bmesh.new()
    revolve(bm, prof, 64, u_splits=2)
    obj = object_from_bmesh("Barrel", bm, [body, rings])
    obj["patina_analytic_uv"] = True
    smooth_by_angle(obj, 30.0)                 # smooth staves, sharp band steps and rims
    return [obj]


def build_hammer():
    wood = make_material("Wood", (0.55, 0.38, 0.20), 0.0, 0.6)
    steel = make_material("Steel", (0.56, 0.57, 0.58), 1.0, 0.35)
    L = 0.34
    prof = [(0.0, 0.0, 0), (0.0165, 0.0, 0), (0.0185, 0.008, 0), (0.0185, 0.05, 0),
            (0.0165, 0.14, 0), (0.0145, 0.24, 0), (0.0150, 0.29, 0), (0.0150, L, 0), (0.0, L, 0)]
    bm = bmesh.new()
    revolve(bm, prof, 32, u_splits=1, max_len=0.12)
    handle = object_from_bmesh("Handle", bm, [wood])
    handle["patina_analytic_uv"] = True
    smooth_by_angle(handle, 35.0)

    bm = bmesh.new()
    bmesh.ops.create_cube(bm, size=1.0)
    bmesh.ops.scale(bm, vec=(0.14, 0.036, 0.042), verts=bm.verts[:])
    for v in bm.verts:                         # slight taper toward the claw end (-X)
        if v.co.x < 0:
            v.co.z *= 0.7
    head = object_from_bmesh("Head", bm, [steel])
    head.location = (0.012, 0.0, L - 0.018)
    add_bevel(head, width=0.003, segments=3)
    apply_modifiers(head)
    return [handle, head]


def build_suzanne():
    skin = make_material("Skin", (0.80, 0.55, 0.45), 0.0, 0.5)
    bm = bmesh.new()
    bmesh.ops.create_monkey(bm, matrix=Matrix.Scale(0.5, 4))    # ~1.4 m wide, like the others
    obj = object_from_bmesh("Suzanne", bm, [skin])
    mod = obj.modifiers.new("Subdivision", "SUBSURF")
    mod.levels = 2
    mod.render_levels = 2
    apply_modifiers(obj)
    obj.data.shade_smooth()
    return [obj]


def build_panel():
    mat = make_material("Panel", (0.30, 0.33, 0.36), 1.0, 0.4)
    T = 0.05
    top = T / 2
    # grooves: (kind, (x0, x1, y0, y1), inner rect for rings, depth)
    grooves = [
        ("ring", (-0.42, 0.42, -0.42, 0.42), (-0.39, 0.39, -0.39, 0.39), 0.012),
        ("rect", (-0.30, 0.06, 0.20, 0.235), None, 0.010),
        ("rect", (-0.30, 0.06, 0.12, 0.155), None, 0.010),
        ("rect", (-0.30, -0.265, -0.32, 0.04), None, 0.010),
        ("rect", (0.12, 0.32, -0.32, -0.08), None, 0.008),     # recessed pocket
        ("rect", (0.12, 0.32, 0.08, 0.30), None, 0.006),       # shallow pocket
    ]
    bm = bmesh.new()
    bmesh.ops.create_cube(bm, size=1.0)
    bmesh.ops.scale(bm, vec=(1.0, 1.0, T), verts=bm.verts[:])
    xs, ys = set(), set()
    for _kind, r, inner, _d in grooves:
        for rr in (r, inner):
            if rr:
                xs.update(rr[:2])
                ys.update(rr[2:])
    for x in sorted(xs):
        bmesh.ops.bisect_plane(bm, geom=bm.verts[:] + bm.edges[:] + bm.faces[:], dist=1e-7,
                               plane_co=(x, 0, 0), plane_no=(1, 0, 0))
    for y in sorted(ys):
        bmesh.ops.bisect_plane(bm, geom=bm.verts[:] + bm.edges[:] + bm.faces[:], dist=1e-7,
                               plane_co=(0, y, 0), plane_no=(0, 1, 0))

    def inside(c, r):
        return r[0] < c.x < r[1] and r[2] < c.y < r[3]

    for kind, r, inner, depth in grooves:
        sel = []
        for f in bm.faces:
            c = f.calc_center_median()
            if f.normal.z > 0.99 and abs(c.z - top) < 1e-5 and inside(c, r):
                if kind == "ring" and inside(c, inner):
                    continue
                sel.append(f)
        ret = bmesh.ops.extrude_face_region(bm, geom=sel)
        verts = [e for e in ret["geom"] if isinstance(e, bmesh.types.BMVert)]
        bmesh.ops.translate(bm, verts=verts, vec=(0.0, 0.0, -depth))
        bmesh.ops.delete(bm, geom=sel, context="FACES")        # drop the old top faces

    # bolt heads at the four corners (sunk 1 mm so they read as seated, no bottom cap)
    for bx in (-0.455, 0.455):
        for by in (-0.455, 0.455):
            h = 0.014
            ret = bmesh.ops.create_cone(bm, cap_ends=True, cap_tris=False, segments=24,
                                        radius1=0.022, radius2=0.022, depth=h,
                                        matrix=Matrix.Translation((bx, by, top - 0.001 + h / 2)))
            vs = set(ret["verts"])
            bottom = [f for f in bm.faces if len(f.verts) > 4 and set(f.verts) <= vs
                      and f.calc_center_median().z < top - 0.0005]
            bmesh.ops.delete(bm, geom=bottom, context="FACES_ONLY")
    obj = object_from_bmesh("Panel", bm, [mat])
    add_bevel(obj, width=0.0025, segments=2)
    apply_modifiers(obj)
    return [obj]


BUILDERS = {"crate": build_crate, "barrel": build_barrel, "hammer": build_hammer,
            "suzanne": build_suzanne, "panel": build_panel}


def main():
    args = parse_args()
    names = [n.strip() for n in args.only.split(",") if n.strip()] or list(ASSET_NAMES)
    bad = [n for n in names if n not in BUILDERS]
    if bad:
        raise RuntimeError("unknown asset(s): %s" % ", ".join(bad))
    OPTS.update(uv_mode=args.uv_mode, check=not args.no_check)
    out_dir = os.path.abspath(args.out)
    os.makedirs(out_dir, exist_ok=True)
    summary = {}
    for name in names:
        reset_scene()
        objs = BUILDERS[name]()
        uv = {o.name: unwrap(o) for o in objs}
        path = os.path.join(out_dir, name + ".glb")
        export_glb(path)
        info = glb_info(path)
        info["uv"] = uv
        summary[name] = info
        if info["triangles"] > 50000:
            print("WARNING: %s has %d triangles (> 50k)" % (name, info["triangles"]))

    print("\n=== Patina test assets (uv-mode %s) -> %s ===" % (args.uv_mode, out_dir))
    for name, s in summary.items():
        print("%-8s %6d tris  %6.1f KB  objects=%s  materials=%s  attrs=%s" % (
            name, s["triangles"], s["bytes"] / 1024.0, ",".join(s["nodes"]),
            ",".join(s["materials"]), ",".join(s["attributes"])))
        for oname, mats in s["uv"].items():
            for mname, r in mats.items():
                if mname.startswith("_"):
                    continue
                ov = (" overlap=%dpx" % r["overlap_px"]) if "overlap_px" in r else ""
                print("         uv %-8s %-12s %-20s tris=%-6d coverage=%.2f density(p5..p95)=%.2f..%.2f%s"
                      % (oname, mname, mats["_method"], r["tris"], r["coverage"],
                         r["texel_density_p5_p95"][0], r["texel_density_p5_p95"][1], ov))
    print("PATINA_JSON " + json.dumps({"ok": True, "out": out_dir, "assets": summary}))


if __name__ == "__main__":
    try:
        main()
    except SystemExit as e:
        if e.code not in (0, None):
            sys.exit(e.code if isinstance(e.code, int) else 2)
    except Exception as e:  # noqa: BLE001 - report every failure as a nonzero exit
        traceback.print_exc()
        print("PATINA_JSON " + json.dumps({"ok": False, "error": "%s: %s" % (type(e).__name__, e)}))
        sys.exit(1)
