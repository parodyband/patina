"""Round-trip smoke test for tools/blender/patina_blender.py.

    /Applications/Blender.app/Contents/MacOS/Blender -b --factory-startup \
        --python tools/blender/test_bridge.py -- [--assets examples/assets] \
        [--dir examples/_bridge_test] [--engines CYCLES,EEVEE] [--keep]

Generates fake Patina texture exports (UV-space procedural PNGs, 16-bit height) + manifests
for crate.glb (separate maps, OpenGL normal) and barrel.glb (two texture sets, packed ORM,
DirectX normal), runs `apply` (with preview renders) and `export` through a child Blender,
checks exit codes / PATINA_JSON payloads / written files, and exercises the error paths.
Exits 1 if any check fails. Test outputs are deleted unless --keep is given.
"""

import argparse
import json
import os
import shutil
import struct
import subprocess
import sys
import zlib

import bpy
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
BRIDGE = os.path.join(HERE, "patina_blender.py")
FAILS = []


def check(cond, msg):
    print(("  ok   " if cond else "  FAIL ") + msg)
    if not cond:
        FAILS.append(msg)


def write_png(path, a):
    """a: (h, w[, c]) uint8 or uint16, row 0 = v=0 (bottom, Blender UV convention)."""
    a = np.ascontiguousarray(np.asarray(a)[::-1])          # PNG stores the top row first
    h, w = a.shape[:2]
    c = 1 if a.ndim == 2 else a.shape[2]
    depth = 16 if a.dtype == np.uint16 else 8
    raw = a.astype(">u2" if depth == 16 else np.uint8).reshape(h, -1)
    body = b"".join(b"\x00" + row.tobytes() for row in raw)

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, depth, {1: 0, 3: 2, 4: 6}[c], 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(body, 6)))
        f.write(chunk(b"IEND", b""))


def fake_maps(res=512, seed=0):
    """Procedural maps in UV space. Height = grid of round studs + ridges; normal derived
    from it so normal and height agree (OpenGL: green = +V)."""
    v, u = np.mgrid[0:res, 0:res].astype(np.float64) / res
    cu, cv = (u * 12) % 1 - 0.5, (v * 12) % 1 - 0.5
    studs = np.clip(1 - np.sqrt(cu ** 2 + cv ** 2) / 0.32, 0, 1) ** 0.6
    ridges = 0.5 + 0.5 * np.sin(2 * np.pi * (u * 3 + v * 2))
    height = 0.5 + 0.35 * studs - 0.1 * ridges
    gy, gx = np.gradient(height * 40.0)                   # exaggerated slope
    n = np.stack([-gx, -gy, np.ones_like(gx)], -1)
    n /= np.linalg.norm(n, axis=-1, keepdims=True)
    checker = ((np.floor(u * 8) + np.floor(v * 8)) % 2)[..., None]
    base = np.where(checker > 0, [0.75, 0.52, 0.30], [0.35, 0.22, 0.12]) * (0.8 + 0.2 * ridges[..., None])
    rough = 0.25 + 0.6 * v
    metal = (((u * 6) % 1) < 0.18).astype(np.float64)
    ao = 0.55 + 0.45 * studs
    emis = np.zeros((res, res, 3))
    emis[(u > 0.47) & (u < 0.5)] = (1.0, 0.45, 0.1)
    opac = np.ones((res, res))
    opac[(u - 0.8) ** 2 + (v - 0.8) ** 2 < 0.004] = 0.0
    to8 = lambda x: np.clip(np.round(x * 255), 0, 255).astype(np.uint8)  # noqa: E731
    return {
        "basecolor": to8(base), "roughness": to8(rough), "metallic": to8(metal),
        "ao": to8(ao), "emissive": to8(emis), "opacity": to8(opac),
        "height": np.clip(np.round(height * 65535), 0, 65535).astype(np.uint16),
        "normal": to8(n * 0.5 + 0.5),
        "normal_dx": to8(n * np.array([0.5, -0.5, 0.5]) + 0.5),
        "orm": to8(np.stack([ao, rough, metal], -1)),
    }


def run_bridge(args, expect_code=0):
    cmd = [bpy.app.binary_path, "-b", "--factory-startup", "--python", BRIDGE, "--"] + args
    p = subprocess.run(cmd, capture_output=True, text=True)
    lines = [l for l in p.stdout.splitlines() if l.startswith("PATINA_JSON ")]
    payload = json.loads(lines[-1][len("PATINA_JSON "):]) if lines else None
    ok = p.returncode == expect_code and payload is not None and len(lines) == 1
    check(ok, "%s -> exit %d (expected %d), %d PATINA_JSON line(s)" % (
        " ".join(args[:1] + [a for a in args[1:] if a.startswith("--")]), p.returncode,
        expect_code, len(lines)))
    if not ok:
        print(p.stdout[-3000:], p.stderr[-3000:])
    return payload or {}


def main():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    ap = argparse.ArgumentParser()
    ap.add_argument("--assets", default=os.path.join(ROOT, "examples", "assets"))
    ap.add_argument("--dir", default=os.path.join(ROOT, "examples", "_bridge_test"))
    ap.add_argument("--engines", default="CYCLES,EEVEE")
    ap.add_argument("--res", type=int, default=512)
    ap.add_argument("--keep", action="store_true")
    a = ap.parse_args(argv)
    d = os.path.abspath(a.dir)
    os.makedirs(d, exist_ok=True)
    crate, barrel = (os.path.join(a.assets, n) for n in ("crate.glb", "barrel.glb"))
    if not (os.path.isfile(crate) and os.path.isfile(barrel)):
        raise SystemExit("run examples/make_assets.py first (missing %s)" % a.assets)

    # --- fake texture exports ---------------------------------------------------------
    maps = fake_maps()
    tex = os.path.join(d, "tex")
    os.makedirs(tex, exist_ok=True)
    for k, arr in maps.items():
        write_png(os.path.join(tex, "T_%s.png" % k), arr)
    sep_files = {k: "tex/T_%s.png" % k for k in
                 ("basecolor", "metallic", "roughness", "normal", "height", "ao", "emissive",
                  "opacity")}
    m1 = {"patina": 1, "preset": "blender", "mesh": os.path.abspath(crate),
          "texture_sets": {"Crate": {"material": "Crate", "files": sep_files,
                                     "normal_format": "opengl", "height_depth": 0.005}}}
    orm_files = {"basecolor": "tex/T_basecolor.png", "orm": "tex/T_orm.png",
                 "normal": "tex/T_normal_dx.png", "height": "tex/T_height.png"}
    m2 = {"patina": 1, "preset": "packed", "mesh": os.path.abspath(barrel),
          "texture_sets": {
              "BarrelBody": {"material": "BarrelBody", "files": orm_files,
                             "normal_format": "directx", "height_depth": 0.004,
                             "normal_includes_height": False},
              "BarrelRings": {"material": "BarrelRings",
                              "files": {"basecolor": "tex/T_basecolor.png",
                                        "orm": "tex/T_orm.png"}}}}
    for name, m in (("manifest.json", m1), ("manifest_orm.json", m2)):
        with open(os.path.join(d, name), "w") as f:
            json.dump(m, f, indent=2)

    # --- apply --------------------------------------------------------------------------
    print("apply: crate, separate maps")
    for eng in [e.strip().upper() for e in a.engines.split(",") if e.strip()]:
        out_blend = os.path.join(d, "crate_%s.blend" % eng.lower())
        png = os.path.join(d, "crate_%s.png" % eng.lower())
        r = run_bridge(["apply", "--manifest", os.path.join(d, "manifest.json"), "--mesh", crate,
                        "--out", out_blend, "--render", png, "--engine", eng,
                        "--samples", "16", "--res", str(a.res)])
        check(r.get("ok") is True, "ok (%s, %ss)" % (eng, r.get("render_seconds")))
        check(os.path.isfile(out_blend) and os.path.isfile(png), "blend + png written")
        ts = r.get("texture_sets", {}).get("Crate", {})
        check(sorted(ts.get("maps", [])) == sorted(sep_files), "all 8 maps wired: %s" % ts.get("maps"))
        check("skipped" in ts.get("height", ""), "height: %s" % ts.get("height"))
        check("multiplied" in ts.get("ao", ""), "ao: %s" % ts.get("ao"))
        if os.path.isfile(png):
            img = bpy.data.images.load(png)
            px = np.array(img.pixels[:]).reshape(-1, img.channels)[:, :3]
            check(px.std() > 0.03, "render is not flat (std %.3f, mean %.3f)" % (px.std(), px.mean()))

    print("apply: barrel, two texture sets, packed ORM + DirectX normal, --blend input")
    r = run_bridge(["apply", "--manifest", os.path.join(d, "manifest_orm.json"),
                    "--out", os.path.join(d, "barrel.blend"),
                    "--render", os.path.join(d, "barrel.png"), "--samples", "16",
                    "--res", str(a.res), "--pack"])
    check(r.get("ok") is True and set(r.get("texture_sets", {})) == {"BarrelBody", "BarrelRings"},
          "both texture sets applied (mesh taken from manifest)")
    check("bump" in r.get("texture_sets", {}).get("BarrelBody", {}).get("height", ""),
          "normal_includes_height=false -> bump chained: %s"
          % r.get("texture_sets", {}).get("BarrelBody", {}).get("height"))
    r = run_bridge(["apply", "--manifest", os.path.join(d, "manifest.json"),
                    "--blend", os.path.join(d, "crate_cycles.blend"),
                    "--out", os.path.join(d, "crate_again.blend"), "--height", "displace"])
    check(r.get("ok") is True and "displacement" in r["texture_sets"]["Crate"]["height"],
          "--blend input + --height displace")

    # inspect the node tree of the saved crate .blend
    bpy.ops.wm.open_mainfile(filepath=os.path.join(d, "crate_cycles.blend"))
    mat = bpy.data.materials["Crate"]
    bsdf = next(n for n in mat.node_tree.nodes if n.type == "BSDF_PRINCIPLED")
    linked = sorted(s.name for s in bsdf.inputs if s.is_linked)
    check(linked == sorted(["Base Color", "Metallic", "Roughness", "Normal", "Alpha",
                            "Emission Color"]), "Principled inputs linked: %s" % linked)
    cs = {n.label: n.image.colorspace_settings.name for n in mat.node_tree.nodes
          if n.type == "TEX_IMAGE"}
    check(cs.get("BaseColor") == "sRGB" and cs.get("Normal") == "Non-Color"
          and cs.get("Height") is None and cs.get("Roughness") == "Non-Color",
          "colour spaces: %s" % cs)
    check(all(i.filepath.startswith("//") for i in bpy.data.images if i.source == "FILE"),
          "image paths relative: %s" % [i.filepath for i in bpy.data.images])

    # --- export -------------------------------------------------------------------------
    print("export")
    r = run_bridge(["export", "--blend", os.path.join(d, "crate_cycles.blend"),
                    "--out", os.path.join(d, "crate_roundtrip.glb")])
    check(r.get("ok") and r.get("materials") == ["Crate"] and r.get("triangles", 0) > 1000,
          "crate round trip: objects=%s materials=%s tris=%s" % (
              r.get("objects"), r.get("materials"), r.get("triangles")))

    # a scene with a UV-less mesh, a subsurf modifier and a hidden object
    bpy.ops.wm.read_factory_settings(use_empty=True)
    import bmesh
    for name, hidden in (("NoUV", False), ("Hidden", True)):
        bm = bmesh.new()
        bmesh.ops.create_cube(bm, size=1.0)
        me = bpy.data.meshes.new(name)
        bm.to_mesh(me)
        bm.free()
        while me.uv_layers:
            me.uv_layers.remove(me.uv_layers[0])
        mats = [bpy.data.materials.new(name + "_A"), bpy.data.materials.new(name + "_B")]
        for m in mats:
            me.materials.append(m)
        for i, p in enumerate(me.polygons):
            p.material_index = i % 2
        o = bpy.data.objects.new(name, me)
        bpy.context.scene.collection.objects.link(o)
        o.modifiers.new("Subsurf", "SUBSURF").levels = 1
        o.hide_set(hidden)
        o.hide_render = hidden
    scene_blend = os.path.join(d, "nouv.blend")
    bpy.ops.wm.save_as_mainfile(filepath=scene_blend)
    r = run_bridge(["export", "--blend", scene_blend, "--out", os.path.join(d, "nouv.glb")])
    check(r.get("ok") and r.get("objects") == ["NoUV"] and r.get("skipped") == ["Hidden"]
          and any("no UV" in w for w in r.get("warnings", [])),
          "hidden skipped, missing-UV warning: %s" % r.get("warnings"))
    check(r.get("triangles") == 6 * 4 * 2, "subsurf applied (triangles=%s)" % r.get("triangles"))
    r = run_bridge(["export", "--blend", scene_blend, "--out", os.path.join(d, "nouv_uv.glb"),
                    "--objects", "NoUV,Hidden", "--auto-uv"])
    check(r.get("ok") and r.get("uv_generated") == ["NoUV", "Hidden"]
          and r.get("materials") == ["NoUV_A", "NoUV_B", "Hidden_A", "Hidden_B"],
          "--objects + --auto-uv: uv_generated=%s" % r.get("uv_generated"))

    # --- error paths ----------------------------------------------------------------------
    print("errors")
    r = run_bridge(["apply", "--manifest", os.path.join(d, "nope.json"), "--mesh", crate,
                    "--out", os.path.join(d, "x.blend")], expect_code=1)
    check(r.get("ok") is False and "manifest not found" in r.get("error", ""), r.get("error", ""))
    bad = dict(m1, texture_sets={"X": {"material": "DoesNotExist", "files": sep_files}})
    with open(os.path.join(d, "bad.json"), "w") as f:
        json.dump(bad, f)
    r = run_bridge(["apply", "--manifest", os.path.join(d, "bad.json"), "--mesh", crate,
                    "--out", os.path.join(d, "x.blend")], expect_code=1)
    check("DoesNotExist" in r.get("error", ""), r.get("error", ""))
    r = run_bridge(["export", "--blend", scene_blend, "--out", os.path.join(d, "x.glb"),
                    "--objects", "Nope"], expect_code=1)
    check("not found" in r.get("error", ""), r.get("error", ""))
    r = run_bridge(["apply", "--out", "x.blend"], expect_code=2)
    check("--manifest" in r.get("error", ""), r.get("error", ""))

    print("\n%d check(s) failed" % len(FAILS) if FAILS else "\nall checks passed")
    if not a.keep:
        shutil.rmtree(d, ignore_errors=True)
    if FAILS:
        sys.exit(1)


if __name__ == "__main__":
    try:
        main()
    except SystemExit:
        raise
    except Exception:  # noqa: BLE001
        import traceback
        traceback.print_exc()
        sys.exit(1)
