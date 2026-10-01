"""Score "match the reference" barrel submissions.

usage: python bench/barrel_ref/score.py <patina.exe> <results_dir> <run_dir> [<run_dir> ...]

For every run: hard checks (deliverables, budgets, one texture set, UV coverage/overlap/density,
slivers, high-poly density, a fresh bake's misses, placement) and standardized renders from the
reference angle and others (lit, wireframe, baked normal, UV layout), in results_dir/<run>/.
"""
import json
import os
import subprocess
import sys

REF = '[{"azimuth":55,"elevation":15}]'
VIEWS = {
    "ref_angle": REF,
    "iso": '["iso"]',
    "back": '["iso_back"]',
    "front_closeup": '[{"azimuth":30,"elevation":12,"zoom":2.4,"target":[0.5,0.5,0.95]}]',
    "top_closeup": '[{"azimuth":40,"elevation":45,"zoom":3,"target":[0.5,1,0.5]}]',
}


def run(cmd):
    p = subprocess.run(cmd, capture_output=True, text=True)
    try:
        return json.loads(p.stdout), p.returncode
    except json.JSONDecodeError:
        return {"ok": False, "error": (p.stdout + p.stderr)[-2000:]}, p.returncode


def score_run(patina, run_dir, out_dir):
    name = os.path.basename(os.path.normpath(run_dir))
    out = os.path.join(out_dir, name)
    os.makedirs(out, exist_ok=True)
    m = {"run": name, "checks": {}, "renders": {}}
    if os.path.isfile(os.path.join(run_dir, "meta.json")):
        m["meta"] = json.load(open(os.path.join(run_dir, "meta.json")))
    low, high = os.path.join(run_dir, "barrel_low.glb"), os.path.join(run_dir, "barrel_high.glb")
    proj = os.path.join(run_dir, "barrel.patina.json")
    c = m["checks"]
    for f in ("barrel_low.glb", "barrel_high.glb", "barrel.patina.json", "renders/hero.png", "NOTES.md"):
        c[f] = os.path.isfile(os.path.join(run_dir, f))
    tex = os.path.join(run_dir, "textures")
    c["textures/"] = os.path.isdir(tex) and len(os.listdir(tex)) > 0
    if c["barrel_low.glb"]:
        info, _ = run([patina, "inspect", low, "--compact"])
        if "triangles" in info:
            b = info["bounds"]
            sets = info["texture_sets"]
            m["low"] = {"triangles": info["triangles"], "texture_sets": list(sets.keys()), "topology": info.get("topology"),
                        "parts": list(info["parts"].keys()), "size": b["size"],
                        "uv": {k: {x: v.get(x) for x in ("uv_area", "uv_overlap_fraction", "texel_density_spread", "mirrored_triangles")}
                               for k, v in sets.items()}}
            s0 = next(iter(sets.values()))
            c["low <= 8k triangles"] = info["triangles"] <= 8000
            c["exactly 1 texture set"] = len(sets) == 1
            c["UV coverage >= 60%"] = len(sets) == 1 and s0["uv_area"] >= 0.6
            c["UV overlap < 0.5%"] = all((v.get("uv_overlap_fraction") or 0) < 0.005 for v in sets.values())
            c["texel density spread < 1.6"] = all((v.get("texel_density_spread") or 99) < 1.6 for v in sets.values())
            c["slivers < 15%"] = (info.get("topology") or {}).get("sliver_fraction", 1) < 0.15
            c["on the ground (min y ~ 0)"] = abs(b["min"][1]) < 0.01
            c["centred (|x|,|z| < 0.1)"] = abs(b["center"][0]) < 0.1 and abs(b["center"][2]) < 0.1
            c["length 0.75..1.2 m front-back"] = 0.75 <= b["size"][2] <= 1.2
            if c["barrel_high.glb"]:
                hi, _ = run([patina, "inspect", high, "--compact"])
                m["high_triangles"] = hi.get("triangles")
                c["high >= 5x low"] = (hi.get("triangles") or 0) >= 5 * info["triangles"]
    if c["barrel.patina.json"]:
        doc = json.load(open(proj))
        c["bake.normal.high set"] = bool(((doc.get("bake") or {}).get("normal") or {}).get("high"))
        v, _ = run([patina, "validate", proj, "--compact"])
        c["project validates"] = bool(v.get("ok"))
        bk, _ = run([patina, "bake", proj, "--force", "--compact"])
        nb = (bk.get("stats") or {}).get("normal") or {}
        m["bake"] = {"miss_fraction": nb.get("miss_fraction"), "groups": nb.get("groups"), "warnings": nb.get("warnings"), "ms": nb.get("ms")}
        c["bake misses < 2%"] = nb.get("miss_fraction") is not None and nb["miss_fraction"] < 0.02
        for vname, views in VIEWS.items():
            r, code = run([patina, "render", proj, "--views", views, "--size", "1000", "--resolution", "2048",
                           "--out", os.path.join(out, vname + ".png"), "--compact"])
            m["renders"][vname] = os.path.basename(r.get("image", "")) if code == 0 else r.get("error")
        for mode in ("wireframe", "bake_normal", "clay"):
            r, code = run([patina, "render", proj, "--views", REF, "--mode", mode, "--size", "1000",
                           "--out", os.path.join(out, mode + ".png"), "--compact"])
            m["renders"][mode] = os.path.basename(r.get("image", "")) if code == 0 else r.get("error")
        r, code = run([patina, "render", proj, "--uv-layout", "--size", "1024", "--out", os.path.join(out, "uv_layout.png"), "--compact"])
        m["renders"]["uv_layout"] = os.path.basename(r.get("image", "")) if code == 0 else r.get("error")
    m["hard_passed"] = sum(1 for v in c.values() if v)
    m["hard_total"] = len(c)
    json.dump(m, open(os.path.join(out, "metrics.json"), "w"), indent=1)
    return m


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    patina, out_dir, runs = sys.argv[1], sys.argv[2], sys.argv[3:]
    os.makedirs(out_dir, exist_ok=True)
    results = [score_run(patina, r, out_dir) for r in runs]
    json.dump(results, open(os.path.join(out_dir, "results.json"), "w"), indent=1)
    lines = ["| run | hard checks | low tris | high tris | UV coverage | slivers | bake misses | minutes |",
             "|---|---|---|---|---|---|---|---|"]
    for m in results:
        low = m.get("low", {})
        uv = next(iter(low.get("uv", {}).values()), {}) if low.get("uv") else {}
        sl = (low.get("topology") or {}).get("sliver_fraction")
        bm = (m.get("bake") or {}).get("miss_fraction")
        meta = m.get("meta", {})
        mins = round((meta["finished"] - meta["started"]) / 60, 1) if "finished" in meta and "started" in meta else "?"
        lines.append(f"| {m['run']} | {m['hard_passed']}/{m['hard_total']} | {low.get('triangles', '-')} | {m.get('high_triangles', '-')} | "
                     f"{'-' if not uv else format(uv.get('uv_area', 0) * 100, '.0f') + '%'} | "
                     f"{'-' if sl is None else f'{sl * 100:.1f}%'} | {'-' if bm is None else f'{bm * 100:.2f}%'} | {mins} |")
    open(os.path.join(out_dir, "results.md"), "w").write("\n".join(lines) + "\n")
    print("\n".join(lines))


if __name__ == "__main__":
    main()
