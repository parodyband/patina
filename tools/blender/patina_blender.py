"""Patina <-> Blender bridge (run inside Blender, background mode).

    BLENDER=/Applications/Blender.app/Contents/MacOS/Blender

    # .blend (or the scene Blender was started with) -> .glb for the Patina engine
    $BLENDER -b --factory-startup --python tools/blender/patina_blender.py -- \
        export --blend IN.blend --out OUT.glb [--objects A,B] [--auto-uv] [--textures]

    # Patina texture export (manifest.json) -> .blend with Principled materials (+ preview PNG)
    $BLENDER -b --factory-startup --python tools/blender/patina_blender.py -- \
        apply --manifest DIR/manifest.json (--mesh IN.glb | --blend IN.blend) --out OUT.blend \
        [--render OUT.png] [--engine CYCLES|EEVEE] [--samples 32] [--res 1024] [--device CPU|GPU]
        [--height auto|bump|displace|skip] [--bump-strength 1.0] [--ao-strength 0.3]
        [--pack] [--allow-missing]

Every run prints exactly one machine-readable line on stdout:
    PATINA_JSON {"ok": true, ...}      or      PATINA_JSON {"ok": false, "error": "..."}
and exits 0 on success, 1 on failure, 2 on bad arguments. (Blender itself exits 0 on an
uncaught Python exception, so every error is caught and turned into sys.exit(1).)

Material mapping decisions (apply):
  * basecolor -> Base Color (sRGB); emissive -> Emission Color (sRGB) with strength 1.
  * metallic / roughness / ao / height / opacity / normal / orm are loaded as Non-Color.
  * orm (packed): Separate Color, R = AO, G = Roughness, B = Metallic. Separate metallic /
    roughness / ao files win over the packed channels when both are present.
  * normal -> Normal Map node (tangent space, UV map = active render UV). Blender expects
    OpenGL (+Y) normals; "normal_format": "directx" flips green in the node tree.
  * AO: the Principled BSDF has no AO input and Cycles/EEVEE compute occlusion themselves, so
    AO is multiplied into Base Color with a low factor (--ao-strength, default 0.3; 0 = unused).
  * height (--height auto, the default): the Patina normal map is assumed to already contain
    the height detail, so height is only turned into bump when there is NO normal map, or when
    the texture set says "normal_includes_height": false (then Bump is chained after the
    Normal Map). Bump node: Distance = height_depth (world units for height 1.0; the 0.5
    neutral level does not matter for a bump gradient), Strength = --bump-strength (default
    1.0, i.e. physically scaled because Distance already is). --height bump forces it,
    --height displace wires a Displacement node (midlevel 0.5, scale = height_depth) to the
    material output with displacement method "Displacement and Bump", --height skip ignores it.
  * opacity -> Alpha (render method Dithered for EEVEE; Cycles needs nothing).
  * Image textures use Extend (not Repeat) so bilinear filtering at the 0/1 UV border does not
    bleed in texels from the opposite side of the atlas.
  * The .blend is saved BEFORE the preview rig is added, so it only differs from the input by
    its materials; image paths are stored relative to the .blend (or packed with --pack).
"""

import argparse
import json
import math
import os
import sys
import time
import traceback

import bpy
from mathutils import Vector

JSON_PREFIX = "PATINA_JSON "
MAP_KEYS = ("basecolor", "metallic", "roughness", "normal", "height", "ao", "emissive",
            "opacity", "orm")


class BridgeError(Exception):
    """An expected failure: reported as {"ok": false, "error": ...} and exit status 1."""


class ArgError(BridgeError):
    """Bad command line: exit status 2."""


class _Parser(argparse.ArgumentParser):
    def error(self, message):
        raise ArgError("%s: %s" % (self.prog, message))


def emit(payload):
    sys.stdout.write(JSON_PREFIX + json.dumps(payload) + "\n")
    sys.stdout.flush()


def log(msg):
    sys.stderr.write("[patina] %s\n" % msg)
    sys.stderr.flush()


# --------------------------------------------------------------------------------------
# scene helpers
# --------------------------------------------------------------------------------------

def ensure_object_mode():
    obj = bpy.context.view_layer.objects.active
    if obj is not None and obj.mode != "OBJECT":
        bpy.ops.object.mode_set(mode="OBJECT")


def open_blend(path):
    path = os.path.abspath(path)
    if not os.path.isfile(path):
        raise BridgeError("blend file not found: %s" % path)
    bpy.ops.wm.open_mainfile(filepath=path, load_ui=False)
    log("opened %s" % path)


def empty_scene():
    bpy.ops.wm.read_factory_settings(use_empty=True)


def import_mesh(path):
    path = os.path.abspath(path)
    if not os.path.isfile(path):
        raise BridgeError("mesh file not found: %s" % path)
    ext = os.path.splitext(path)[1].lower()
    if ext in (".glb", ".gltf"):
        bpy.ops.import_scene.gltf(filepath=path)
    elif ext == ".fbx":
        bpy.ops.import_scene.fbx(filepath=path)
    elif ext == ".obj":
        bpy.ops.wm.obj_import(filepath=path)
    else:
        raise BridgeError("unsupported mesh format %r (use .glb/.gltf/.fbx/.obj)" % ext)
    log("imported %s" % path)


def mesh_objects(scene):
    return [o for o in scene.objects if o.type == "MESH"]


def triangle_count(objs):
    dg = bpy.context.evaluated_depsgraph_get()
    total = 0
    for o in objs:
        ev = o.evaluated_get(dg)
        me = ev.to_mesh()
        try:
            me.calc_loop_triangles()
            total += len(me.loop_triangles)
        finally:
            ev.to_mesh_clear()
    return total


def material_names(objs):
    names = []
    for o in objs:
        for slot in o.material_slots:
            if slot.material and slot.material.name not in names:
                names.append(slot.material.name)
    return names


def auto_uv(obj):
    """Smart UV Project (66 deg, margin 0.02) -> seams from islands -> minimum-stretch relax ->
    pack, separately per material so every texture set gets its own full 0-1 space."""
    import bmesh
    me = obj.data
    if not me.uv_layers:
        me.uv_layers.new(name="UVMap")
    for o in bpy.context.view_layer.objects:
        o.select_set(False)
    obj.hide_set(False)
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj
    ts = bpy.context.scene.tool_settings
    old_sync = ts.use_uv_select_sync
    ts.use_uv_select_sync = False     # 5.x default is True: UV ops would touch every face
    bpy.ops.object.mode_set(mode="EDIT")
    try:
        bm = bmesh.from_edit_mesh(me)
        bm.select_mode = {"FACE"}
        for mi in sorted({f.material_index for f in bm.faces}):
            for f in bm.faces:
                f.select_set(False)
            for f in bm.faces:
                if f.material_index == mi:
                    f.select_set(True)
            bm.select_flush_mode()
            bmesh.update_edit_mesh(me)
            bpy.ops.uv.smart_project(angle_limit=math.radians(66.0), island_margin=0.02,
                                     area_weight=0.0, correct_aspect=True, scale_to_bounds=False)
            bpy.ops.uv.select_all(action="SELECT")
            bpy.ops.uv.seams_from_islands(mark_seams=True, mark_sharp=False)
            bpy.ops.uv.unwrap(method="MINIMUM_STRETCH", fill_holes=True, correct_aspect=True,
                              margin_method="FRACTION", margin=0.006)
            bpy.ops.uv.select_all(action="SELECT")
            bpy.ops.uv.pack_islands(rotate=True, scale=True, margin_method="FRACTION",
                                    margin=0.006, shape_method="CONCAVE")
        for f in bm.faces:
            f.select_set(False)
        bmesh.update_edit_mesh(me)
    finally:
        bpy.ops.object.mode_set(mode="OBJECT")
        ts.use_uv_select_sync = old_sync


# --------------------------------------------------------------------------------------
# export
# --------------------------------------------------------------------------------------

def cmd_export(args):
    if args.blend:
        open_blend(args.blend)
    ensure_object_mode()
    scene = bpy.context.scene
    warnings, skipped = [], []

    if args.objects:
        objs = []
        for name in [n.strip() for n in args.objects.split(",") if n.strip()]:
            o = scene.objects.get(name)
            if o is None:
                raise BridgeError("object %r not found in scene %r (mesh objects: %s)" % (
                    name, scene.name, ", ".join(o.name for o in mesh_objects(scene))))
            if o.type != "MESH":
                raise BridgeError("object %r is a %s, not a mesh" % (name, o.type))
            if o.name not in bpy.context.view_layer.objects:
                raise BridgeError("object %r is in a collection excluded from view layer %r"
                                  % (name, bpy.context.view_layer.name))
            objs.append(o)
    else:
        objs = []
        for o in mesh_objects(scene):
            if o.hide_render or o.hide_viewport or not o.visible_get():
                skipped.append(o.name)
            else:
                objs.append(o)
    if not objs:
        raise BridgeError("no mesh objects to export")

    for o in objs:                      # explicitly requested objects may be hidden
        o.hide_viewport = False
        o.hide_set(False)

    uv_generated = []
    done_meshes = set()
    for o in objs:
        if o.data.uv_layers:
            continue
        if not args.auto_uv:
            warnings.append("%s has no UV map (use --auto-uv)" % o.name)
            continue
        if o.data.library is not None:
            warnings.append("%s: linked mesh data, cannot unwrap" % o.name)
            continue
        if o.data.name in done_meshes:
            continue
        auto_uv(o)
        done_meshes.add(o.data.name)
        uv_generated.append(o.name)

    for o in bpy.context.view_layer.objects:
        o.select_set(False)
    for o in objs:
        o.select_set(True)
    bpy.context.view_layer.objects.active = objs[0]

    out = os.path.abspath(args.out)
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    fmt = "GLTF_SEPARATE" if out.lower().endswith(".gltf") else "GLB"
    bpy.ops.export_scene.gltf(
        filepath=out, export_format=fmt, use_selection=True, export_apply=True,
        export_yup=True, export_normals=True, export_texcoords=True, export_tangents=False,
        export_materials="EXPORT", export_image_format="AUTO" if args.textures else "NONE",
        export_cameras=False, export_lights=False, export_animations=False,
        export_extras=False, export_skins=False, export_morph=False)
    if not os.path.isfile(out):
        raise BridgeError("glTF exporter did not write %s" % out)
    no_mat = [o.name for o in objs if not any(s.material for s in o.material_slots)]
    if no_mat:
        warnings.append("objects without material (single default texture set): %s"
                        % ", ".join(no_mat))
    emit({"ok": True, "command": "export", "out": out, "objects": [o.name for o in objs],
          "materials": material_names(objs), "triangles": triangle_count(objs),
          "uv_generated": uv_generated, "skipped": skipped, "warnings": warnings})


# --------------------------------------------------------------------------------------
# apply
# --------------------------------------------------------------------------------------

def find_material(name):
    m = bpy.data.materials.get(name)
    if m is not None:
        return m
    lname = name.lower()
    for m in bpy.data.materials:        # "Crate.001" (import name clash) or different case
        base = m.name.rsplit(".", 1)[0] if m.name[-4:-3] == "." and m.name[-3:].isdigit() \
            else m.name
        if base.lower() == lname:
            return m
    return None


class NodeBuilder:
    def __init__(self, mat):
        if mat.node_tree is None:       # Blender < 5.0
            mat.use_nodes = True
        self.mat = mat
        self.nt = mat.node_tree
        self.nt.nodes.clear()

    def node(self, idname, x, y, label=None, **props):
        n = self.nt.nodes.new(idname)
        n.location = (x, y)
        if label:
            n.label = label
        for k, v in props.items():
            setattr(n, k, v)
        return n

    def link(self, out_sock, in_sock):
        self.nt.links.new(out_sock, in_sock)

    def image(self, path, colorspace, x, y, label):
        img = bpy.data.images.load(path, check_existing=True)
        img.colorspace_settings.name = colorspace
        n = self.node("ShaderNodeTexImage", x, y, label=label)
        n.image = img
        n.extension = "EXTEND"
        n.interpolation = "Linear"
        return n


def sock(sockets, identifier):
    for s in sockets:
        if s.identifier == identifier:
            return s
    raise BridgeError("socket %r not found" % identifier)


def build_material(mat, ts, mdir, opts):
    info = {"material": mat.name, "maps": [], "missing_files": [], "ignored_keys": []}
    files = {}
    for key, rel in (ts.get("files") or {}).items():
        k = key.lower()
        if k not in MAP_KEYS:
            info["ignored_keys"].append(key)
            continue
        if not rel:
            continue
        path = rel if os.path.isabs(rel) else os.path.join(mdir, rel)
        if os.path.isfile(path):
            files[k] = os.path.abspath(path)
        else:
            info["missing_files"].append(path)

    b = NodeBuilder(mat)
    out = b.node("ShaderNodeOutputMaterial", 700, 300)
    bsdf = b.node("ShaderNodeBsdfPrincipled", 300, 300)
    b.link(bsdf.outputs["BSDF"], out.inputs["Surface"])
    X = -700                                   # texture column
    y = [900]

    def next_y(step=300):
        y[0] -= step
        return y[0] + step

    ao_src = rough_src = metal_src = None
    if "orm" in files:
        t = b.image(files["orm"], "Non-Color", X, next_y(), "ORM")
        sep = b.node("ShaderNodeSeparateColor", X + 330, t.location.y, label="ORM split")
        sep.mode = "RGB"
        b.link(t.outputs["Color"], sep.inputs["Color"])
        ao_src, rough_src, metal_src = sep.outputs["Red"], sep.outputs["Green"], sep.outputs["Blue"]
    if "ao" in files:
        ao_src = b.image(files["ao"], "Non-Color", X, next_y(), "AO").outputs["Color"]
    if "roughness" in files:
        rough_src = b.image(files["roughness"], "Non-Color", X, next_y(), "Roughness").outputs["Color"]
    if "metallic" in files:
        metal_src = b.image(files["metallic"], "Non-Color", X, next_y(), "Metallic").outputs["Color"]

    if "basecolor" in files:
        t = b.image(files["basecolor"], "sRGB", X, next_y(), "BaseColor")
        col = t.outputs["Color"]
        if ao_src is not None and opts.ao_strength > 0:
            mix = b.node("ShaderNodeMix", X + 330, t.location.y, label="AO multiply",
                         data_type="RGBA", blend_type="MULTIPLY", clamp_result=True)
            sock(mix.inputs, "Factor_Float").default_value = opts.ao_strength
            b.link(col, sock(mix.inputs, "A_Color"))
            b.link(ao_src, sock(mix.inputs, "B_Color"))
            col = sock(mix.outputs, "Result_Color")
            info["ao"] = "multiplied into base color x%.2f" % opts.ao_strength
        b.link(col, bsdf.inputs["Base Color"])
    elif ao_src is not None:
        info["ao"] = "unused (no basecolor)"
    if ao_src is not None and "ao" not in info:
        info["ao"] = "unused (--ao-strength 0)"
    if rough_src is not None:
        b.link(rough_src, bsdf.inputs["Roughness"])
    if metal_src is not None:
        b.link(metal_src, bsdf.inputs["Metallic"])

    normal_out = None
    nfmt = str(ts.get("normal_format", "opengl")).lower()
    info["normal_format"] = nfmt
    if "normal" in files:
        t = b.image(files["normal"], "Non-Color", X, next_y(), "Normal")
        col = t.outputs["Color"]
        if nfmt == "directx":                  # flip green: DirectX (-Y) -> OpenGL (+Y)
            sep = b.node("ShaderNodeSeparateColor", X + 300, t.location.y, label="DX->GL")
            inv = b.node("ShaderNodeMath", X + 480, t.location.y, operation="SUBTRACT")
            comb = b.node("ShaderNodeCombineColor", X + 660, t.location.y)
            inv.inputs[0].default_value = 1.0
            b.link(col, sep.inputs["Color"])
            b.link(sep.outputs["Green"], inv.inputs[1])
            b.link(sep.outputs["Red"], comb.inputs["Red"])
            b.link(inv.outputs["Value"], comb.inputs["Green"])
            b.link(sep.outputs["Blue"], comb.inputs["Blue"])
            col = comb.outputs["Color"]
        elif nfmt != "opengl":
            raise BridgeError("unknown normal_format %r (opengl|directx)" % nfmt)
        nm = b.node("ShaderNodeNormalMap", -120, t.location.y, space="TANGENT")
        nm.inputs["Strength"].default_value = 1.0
        b.link(col, nm.inputs["Color"])
        normal_out = nm.outputs["Normal"]

    depth = float(ts.get("height_depth", 0.005))
    if "height" in files:
        mode = opts.height
        if mode == "auto":
            if normal_out is None:
                mode = "bump"
            elif ts.get("normal_includes_height") is False:
                mode = "bump"
            else:
                mode = "skip"
                info["height"] = "skipped (normal map present and assumed to include height)"
        if mode in ("bump", "displace"):
            t = b.image(files["height"], "Non-Color", X, next_y(), "Height")
        if mode == "bump":
            bump = b.node("ShaderNodeBump", -120, t.location.y - 250)
            bump.inputs["Strength"].default_value = opts.bump_strength
            bump.inputs["Distance"].default_value = depth
            b.link(t.outputs["Color"], bump.inputs["Height"])
            if normal_out is not None:
                b.link(normal_out, bump.inputs["Normal"])
            normal_out = bump.outputs["Normal"]
            info["height"] = "bump (strength %.2f, distance %g)" % (opts.bump_strength, depth)
        elif mode == "displace":
            disp = b.node("ShaderNodeDisplacement", 300, -200, space="WORLD")
            disp.inputs["Midlevel"].default_value = 0.5
            disp.inputs["Scale"].default_value = depth
            b.link(t.outputs["Color"], disp.inputs["Height"])
            b.link(disp.outputs["Displacement"], out.inputs["Displacement"])
            mat.displacement_method = "BOTH"
            info["height"] = "displacement (midlevel 0.5, scale %g, method BOTH)" % depth
        elif mode == "skip" and "height" not in info:
            info["height"] = "skipped (--height skip)"
    if normal_out is not None:
        b.link(normal_out, bsdf.inputs["Normal"])

    if "emissive" in files:
        t = b.image(files["emissive"], "sRGB", X, next_y(), "Emissive")
        b.link(t.outputs["Color"], bsdf.inputs["Emission Color"])
        bsdf.inputs["Emission Strength"].default_value = 1.0
    if "opacity" in files:
        t = b.image(files["opacity"], "Non-Color", X, next_y(), "Opacity")
        b.link(t.outputs["Color"], bsdf.inputs["Alpha"])
        if hasattr(mat, "surface_render_method"):
            mat.surface_render_method = "DITHERED"

    info["maps"] = sorted(files)
    return info


# ---- preview render ------------------------------------------------------------------

def world_bbox(objs):
    dg = bpy.context.evaluated_depsgraph_get()
    lo = Vector((math.inf,) * 3)
    hi = Vector((-math.inf,) * 3)
    for o in objs:
        ev = o.evaluated_get(dg)
        for c in ev.bound_box:
            w = ev.matrix_world @ Vector(c)
            lo = Vector(map(min, lo, w))
            hi = Vector(map(max, hi, w))
    return lo, hi


def setup_preview(scene, objs, args):
    """Studio rig in its own collection: 3/4 camera framing the bbox, sun key + area rim,
    neutral grey world. Other lights are hidden from the render (the rig is not saved)."""
    for o in scene.objects:
        if o.type == "LIGHT":
            o.hide_render = True
    lo, hi = world_bbox(objs)
    center = (lo + hi) * 0.5
    radius = max((hi - lo).length * 0.5, 1e-3)

    coll = bpy.data.collections.new("PatinaPreview")
    scene.collection.children.link(coll)

    cam_data = bpy.data.cameras.new("PatinaCam")
    cam_data.lens = 50.0
    cam_data.sensor_width = 36.0
    cam_data.sensor_fit = "AUTO"
    fov = 2.0 * math.atan(cam_data.sensor_width * 0.5 / cam_data.lens)
    dist = radius / math.sin(fov * 0.5) * 1.08
    az, el = math.radians(35.0), math.radians(24.0)       # 3/4 view from front-right, above
    direction = Vector((math.sin(az) * math.cos(el), -math.cos(az) * math.cos(el), math.sin(el)))
    cam = bpy.data.objects.new("PatinaCam", cam_data)
    cam.location = center + direction * dist
    cam.rotation_mode = "QUATERNION"
    cam.rotation_quaternion = (center - cam.location).to_track_quat("-Z", "Y")
    cam_data.clip_start = max(dist * 0.01, 1e-4)
    cam_data.clip_end = dist * 10.0
    coll.objects.link(cam)
    scene.camera = cam

    sun_data = bpy.data.lights.new("PatinaKey", "SUN")
    sun_data.energy = 3.0
    sun_data.angle = math.radians(8.0)
    sun = bpy.data.objects.new("PatinaKey", sun_data)
    key_dir = Vector((-0.55, -0.7, 0.65)).normalized()     # light comes from front-left, above
    sun.rotation_mode = "QUATERNION"
    sun.rotation_quaternion = (-key_dir).to_track_quat("-Z", "Y")
    coll.objects.link(sun)

    rim_data = bpy.data.lights.new("PatinaRim", "AREA")
    rim_d = radius * 3.0
    rim_data.size = radius * 2.0
    rim_data.energy = 12.0 * rim_d * rim_d
    rim = bpy.data.objects.new("PatinaRim", rim_data)
    rim.location = center + Vector((0.6, 0.8, 0.5)).normalized() * rim_d
    rim.rotation_mode = "QUATERNION"
    rim.rotation_quaternion = (center - rim.location).to_track_quat("-Z", "Y")
    coll.objects.link(rim)

    world = bpy.data.worlds.new("PatinaPreviewWorld")
    if world.node_tree is None:
        world.use_nodes = True
    bg = next((n for n in world.node_tree.nodes if n.type == "BACKGROUND"), None)
    if bg is None:
        bg = world.node_tree.nodes.new("ShaderNodeBackground")
        wout = next(n for n in world.node_tree.nodes if n.type == "OUTPUT_WORLD")
        world.node_tree.links.new(bg.outputs["Background"], wout.inputs["Surface"])
    bg.inputs["Color"].default_value = (0.18, 0.18, 0.18, 1.0)
    bg.inputs["Strength"].default_value = 0.8
    world.color = (0.18, 0.18, 0.18)
    scene.world = world

    r = scene.render
    r.resolution_x = args.res
    r.resolution_y = args.res
    r.resolution_percentage = 100
    r.film_transparent = False
    r.image_settings.file_format = "PNG"
    r.image_settings.color_mode = "RGB"
    r.image_settings.color_depth = "8"
    vs = scene.view_settings
    for vt in ("AgX", "Filmic", "Standard"):
        try:
            vs.view_transform = vt
            break
        except TypeError:
            continue
    try:
        vs.look = "None"
    except TypeError:
        pass
    vs.exposure = 0.0
    vs.gamma = 1.0

    if args.engine == "CYCLES":
        r.engine = "CYCLES"
        cyc = scene.cycles
        cyc.samples = args.samples
        cyc.use_adaptive_sampling = True
        cyc.use_denoising = True
        try:
            cyc.denoiser = "OPENIMAGEDENOISE"
        except TypeError:
            pass
        cyc.device = "CPU"
        if args.device == "GPU":
            cyc.device = "GPU" if enable_gpu() else "CPU"
    else:
        r.engine = "BLENDER_EEVEE"
        scene.eevee.taa_render_samples = args.samples
    return {"camera": list(cam.location), "center": list(center), "radius": radius}


def enable_gpu():
    try:
        prefs = bpy.context.preferences.addons["cycles"].preferences
    except KeyError:
        return False
    for backend in ("METAL", "OPTIX", "CUDA", "HIP", "ONEAPI"):
        try:
            prefs.compute_device_type = backend
        except TypeError:
            continue
        prefs.get_devices()
        devs = [d for d in prefs.devices if d.type == backend]
        if devs:
            for d in prefs.devices:
                d.use = d.type == backend
            log("Cycles GPU backend: %s (%s)" % (backend, ", ".join(d.name for d in devs)))
            return True
    log("no Cycles GPU device found, rendering on CPU")
    return False


def cmd_apply(args):
    manifest_path = os.path.abspath(args.manifest)
    if not os.path.isfile(manifest_path):
        raise BridgeError("manifest not found: %s" % manifest_path)
    try:
        with open(manifest_path, "r", encoding="utf-8") as f:
            manifest = json.load(f)
    except ValueError as e:
        raise BridgeError("manifest is not valid JSON: %s" % e)
    sets = manifest.get("texture_sets")
    if not isinstance(sets, dict) or not sets:
        raise BridgeError("manifest has no texture_sets")
    mdir = os.path.dirname(manifest_path)

    if args.blend:
        open_blend(args.blend)
        source = os.path.abspath(args.blend)
    else:
        mesh = args.mesh or manifest.get("mesh")
        if not mesh:
            raise BridgeError("no input: pass --mesh or --blend (manifest has no \"mesh\")")
        if not os.path.isabs(mesh):
            mesh = os.path.join(mdir, mesh) if not args.mesh else os.path.abspath(mesh)
        empty_scene()
        import_mesh(mesh)
        source = os.path.abspath(mesh)
    ensure_object_mode()
    scene = bpy.context.scene

    warnings, results, missing = [], {}, []
    for set_name, ts in sets.items():
        if not isinstance(ts, dict):
            raise BridgeError("texture set %r is not an object" % set_name)
        mname = ts.get("material") or set_name
        mat = find_material(mname)
        if mat is None:
            missing.append(mname)
            continue
        info = build_material(mat, ts, mdir, args)
        results[set_name] = info
        for p in info["missing_files"]:
            warnings.append("%s: file not found %s" % (set_name, p))
        if info["ignored_keys"]:
            warnings.append("%s: ignored unknown map keys %s" % (set_name, info["ignored_keys"]))
        if not info["maps"]:
            warnings.append("%s: no texture files found, material left untextured" % set_name)
    if missing:
        msg = "material(s) not found: %s (available: %s)" % (
            ", ".join(missing), ", ".join(m.name for m in bpy.data.materials))
        if not args.allow_missing:
            raise BridgeError(msg)
        warnings.append(msg)
    if not results:
        raise BridgeError("no texture set could be applied")

    out = os.path.abspath(args.out)
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    bpy.context.preferences.filepaths.save_version = 0      # no .blend1 backups
    if args.pack:
        bpy.ops.file.pack_all()
    bpy.ops.wm.save_as_mainfile(filepath=out, check_existing=False)
    if not args.pack:
        try:
            bpy.ops.file.make_paths_relative()
        except RuntimeError as e:                 # e.g. different drive on Windows
            warnings.append("could not make image paths relative: %s" % e)
        bpy.ops.wm.save_mainfile(check_existing=False)
    log("saved %s" % out)

    objs = [o for o in mesh_objects(scene) if not o.hide_render]
    payload = {"ok": True, "command": "apply", "blend": out, "source": source,
               "manifest": manifest_path, "texture_sets": results,
               "objects": [o.name for o in objs], "warnings": warnings, "render": None}
    if args.render:
        if not objs:
            raise BridgeError("nothing to render (no visible mesh objects)")
        rig = setup_preview(scene, objs, args)
        png = os.path.abspath(args.render)
        os.makedirs(os.path.dirname(png) or ".", exist_ok=True)
        scene.render.filepath = png
        t0 = time.time()
        bpy.ops.render.render(write_still=True)
        if not os.path.isfile(png):
            raise BridgeError("render finished but %s was not written" % png)
        payload.update(render=png, engine=scene.render.engine,
                       device=getattr(scene.cycles, "device", None)
                       if scene.render.engine == "CYCLES" else "GPU",
                       samples=args.samples, res=args.res,
                       render_seconds=round(time.time() - t0, 2), camera=rig["camera"])
    emit(payload)


# --------------------------------------------------------------------------------------

def build_parser():
    p = _Parser(prog="patina_blender.py",
                description="Patina <-> Blender bridge. Run: Blender -b --factory-startup "
                            "--python patina_blender.py -- <command> ...")
    sub = p.add_subparsers(dest="cmd", required=True, parser_class=_Parser)

    e = sub.add_parser("export", help="export mesh objects of a .blend to .glb")
    e.add_argument("--blend", help="input .blend (default: the scene Blender was started with)")
    e.add_argument("--out", required=True, help="output .glb (or .gltf)")
    e.add_argument("--objects", help="comma separated object names (default: all visible meshes)")
    e.add_argument("--auto-uv", action="store_true",
                   help="smart-UV-unwrap objects without a UV map (per material, packed)")
    e.add_argument("--textures", action="store_true", help="also embed image textures")

    a = sub.add_parser("apply", help="build Principled materials from a Patina manifest")
    a.add_argument("--manifest", required=True)
    src = a.add_mutually_exclusive_group()
    src.add_argument("--mesh", help=".glb/.gltf/.fbx/.obj to import (default: manifest \"mesh\")")
    src.add_argument("--blend", help=".blend to open instead of importing a mesh")
    a.add_argument("--out", required=True, help="output .blend")
    a.add_argument("--render", help="also render a preview PNG to this path")
    a.add_argument("--engine", default="CYCLES", type=str.upper, choices=("CYCLES", "EEVEE"))
    a.add_argument("--device", default="CPU", type=str.upper, choices=("CPU", "GPU"),
                   help="Cycles device (GPU = Metal/OptiX/CUDA/HIP/oneAPI if available)")
    a.add_argument("--samples", type=int, default=32)
    a.add_argument("--res", type=int, default=1024)
    a.add_argument("--height", default="auto", choices=("auto", "bump", "displace", "skip"))
    a.add_argument("--bump-strength", type=float, default=1.0)
    a.add_argument("--ao-strength", type=float, default=0.3)
    a.add_argument("--pack", action="store_true", help="pack images into the .blend")
    a.add_argument("--allow-missing", action="store_true",
                   help="warn instead of failing when a manifest material is not in the scene")
    return p


def main():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    args = build_parser().parse_args(argv)
    if args.cmd == "apply" and (args.samples < 1 or args.res < 16):
        raise ArgError("--samples must be >= 1 and --res >= 16")
    {"export": cmd_export, "apply": cmd_apply}[args.cmd](args)


if __name__ == "__main__":
    try:
        main()
    except SystemExit as e:                 # --help
        sys.exit(e.code if isinstance(e.code, int) else 0)
    except ArgError as e:
        emit({"ok": False, "error": str(e)})
        sys.exit(2)
    except BridgeError as e:
        emit({"ok": False, "error": str(e)})
        sys.exit(1)
    except Exception as e:  # noqa: BLE001
        traceback.print_exc()
        emit({"ok": False, "error": "%s: %s" % (type(e).__name__, e)})
        sys.exit(1)
