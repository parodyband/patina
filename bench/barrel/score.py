"""Score barrel benchmark submissions.

usage: python bench/barrel/score.py <patina.exe> <results_dir> <run_dir> [<run_dir> ...]

Each run_dir is a workspace that received TASK.md. For every run this writes, under results_dir/<run name>/:
standardized renders (same cameras and lighting for everyone, plus wireframes and UV layouts) and
metrics.json; then results.json and
results.md for all runs. A run may contain meta.json ({"model": ..., "started": ..., "finished": ...,
"exit_code": ...}) written by the harness.
"""
import json
import os
import subprocess
import sys

VIEWS = {
    "hero": '["iso"]',
    "back": '["iso_back"]',
    "front_closeup": '[{"azimuth":20,"elevation":12,"zoom":2.4,"target":[0.5,0.45,0.95]}]',
    "top_closeup": '[{"azimuth":30,"elevation":50,"zoom":3,"target":[0.5,1,0.5]}]',
    "side": '["right"]',
}


def run(cmd, cwd=None):
    p = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
    try:
        return json.loads(p.stdout), p.returncode
    except json.JSONDecodeError:
        return {"ok": False, "error": (p.stdout + p.stderr)[-2000:]}, p.returncode


def score_run(patina, run_dir, out_dir):
    name = os.path.basename(os.path.normpath(run_dir))
    out = os.path.join(out_dir, name)
    os.makedirs(out, exist_ok=True)
    m = {"run": name, "checks": {}, "renders": {}}
    meta_path = os.path.join(run_dir, "meta.json")
    if os.path.isfile(meta_path):
        m["meta"] = json.load(open(meta_path))
    glb = os.path.join(run_dir, "barrel.glb")
    proj = os.path.join(run_dir, "barrel.patina.json")
    c = m["checks"]
    c["barrel.glb"] = os.path.isfile(glb)
    c["barrel.patina.json"] = os.path.isfile(proj)
    c["textures/"] = os.path.isdir(os.path.join(run_dir, "textures")) and len(os.listdir(os.path.join(run_dir, "textures"))) > 0
    c["renders/hero.png"] = os.path.isfile(os.path.join(run_dir, "renders", "hero.png"))
    c["NOTES.md"] = os.path.isfile(os.path.join(run_dir, "NOTES.md"))
    if c["barrel.glb"]:
        info, _ = run([patina, "inspect", glb, "--compact"])
        if "triangles" in info:
            b = info["bounds"]
            m["mesh"] = {"triangles": info["triangles"], "height": b["size"][1], "min_y": b["min"][1],
                         "center_xz": [b["center"][0], b["center"][2]], "parts": list(info["parts"].keys()),
                         "texture_sets": {}}
            for sname, s in info["texture_sets"].items():
                m["mesh"]["texture_sets"][sname] = {k: s.get(k) for k in ("triangles", "uv_area", "uv_overlap_fraction",
                                                                         "texel_density_p5_p95", "texel_density_spread", "warnings")}
            sets = m["mesh"]["texture_sets"].values()
            c["triangles <= 60k"] = info["triangles"] <= 60000
            c[">= 5 texture sets"] = len(info["texture_sets"]) >= 5
            c["length 0.75..1.2 m front-back"] = 0.75 <= b["size"][2] <= 1.2
            c["on the ground (min y ~ 0)"] = abs(b["min"][1]) < 0.01
            c["centred (|x|,|z| < 0.1)"] = abs(b["center"][0]) < 0.1 and abs(b["center"][2]) < 0.1
            c["no UV overlap (< 0.1%)"] = all((s["uv_overlap_fraction"] or 0) < 0.001 for s in sets)
            c["texel density spread < 2"] = all((s["texel_density_spread"] or 99) < 2.0 for s in sets)
        else:
            m["inspect_error"] = info
    if c["barrel.patina.json"]:
        v, _ = run([patina, "validate", proj, "--compact"])
        c["project validates"] = bool(v.get("ok"))
        m["validate"] = v
        for vname, views in VIEWS.items():
            r, code = run([patina, "render", proj, "--views", views, "--size", "900", "--resolution", "2048",
                           "--out", os.path.join(out, vname + ".png"), "--compact"])
            m["renders"][vname] = os.path.basename(r.get("image", "")) if code == 0 else r.get("error")
        for mode in ("clay", "uv_checker", "wireframe"):
            r, code = run([patina, "render", proj, "--views", "iso,iso_back", "--mode", mode, "--size", "600",
                           "--out", os.path.join(out, mode + ".png"), "--compact"])
            m["renders"][mode] = os.path.basename(r.get("image", "")) if code == 0 else r.get("error")
        r, code = run([patina, "render", proj, "--views", VIEWS["front_closeup"], "--mode", "wireframe", "--size", "900",
                       "--out", os.path.join(out, "wireframe_closeup.png"), "--compact"])
        m["renders"]["wireframe_closeup"] = os.path.basename(r.get("image", "")) if code == 0 else r.get("error")
        r, code = run([patina, "render", proj, "--uv-layout", "--size", "512", "--out", os.path.join(out, "uv_layout.png"), "--compact"])
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
    lines = ["| run | hard checks | triangles | texture sets | max UV overlap | max density spread | minutes |", "|---|---|---|---|---|---|---|"]
    for m in results:
        mesh = m.get("mesh", {})
        sets = mesh.get("texture_sets", {}).values()
        ov = max((s["uv_overlap_fraction"] or 0 for s in sets), default=None)
        sp = max((s["texel_density_spread"] or 0 for s in sets), default=None)
        meta = m.get("meta", {})
        mins = round((meta["finished"] - meta["started"]) / 60, 1) if "finished" in meta and "started" in meta else "?"
        lines.append(f"| {m['run']} | {m['hard_passed']}/{m['hard_total']} | {mesh.get('triangles', '-')} | {len(mesh.get('texture_sets', {}))} | "
                     f"{'-' if ov is None else f'{ov * 100:.2f}%'} | {'-' if sp is None else round(sp, 2)} | {mins} |")
    open(os.path.join(out_dir, "results.md"), "w").write("\n".join(lines) + "\n")
    print("\n".join(lines))


if __name__ == "__main__":
    main()
