#!/usr/bin/env bash
# End-to-end tests for the patina binary. usage: tests/run_tests.sh [path/to/patina]
set -euo pipefail
cd "$(dirname "$0")/.."
BIN=${1:-build/patina}
# Windows often has only `python` (and a Store stub named python3 that fails).
if [ -z "${PY:-}" ]; then python3 -c "" 2>/dev/null && PY=python3 || PY=python; fi
W=ci_work
export PATINA_NO_UPDATE_CHECK=1  # no network calls from the MCP server during tests
rm -rf "$W" && mkdir -p "$W"
pass=0
ok() { pass=$((pass + 1)); echo "  ok  $1"; }
fail() { echo "  FAIL $1"; exit 1; }
json() { "$PY" -c "import json,sys; d=json.load(sys.stdin); print($1)"; }

echo "== patina tests ($BIN)"
"$BIN" version >/dev/null && ok "version"

# inspect every example asset
for a in crate barrel hammer suzanne panel; do
  tris=$("$BIN" inspect "examples/assets/$a.glb" --compact | json "d['triangles']")
  [ "$tris" -gt 0 ] || fail "inspect $a"
done
ok "inspect 5 assets"

# new + edit + validate
"$BIN" new "$W/crate.patina.json" --mesh examples/assets/crate.glb --resolution 512 --smart painted_metal --compact >/dev/null || fail "new"
"$BIN" edit "$W/crate.patina.json" '[{"op":"add","layer":{"id":"dust","type":"smart","material":"dust_overlay"}},
  {"op":"add","layer":{"id":"stripe","channels":{"basecolor":"#d8c21a"},"mask":[{"type":"paint","strokes":[{"points":[[0,0.5,1],[1,0.5,1]],"radius":0.03}]}]}},
  {"op":"update","id":"painted_metal","patch":{"params":{"wear":0.7}}},
  {"op":"move","id":"stripe","index":0}]' --compact >/dev/null || fail "edit"
ok "new + edit"
[ "$("$BIN" validate "$W/crate.patina.json" --compact | json "d['ok']")" = "True" ] || fail "validate"
ok "validate"

# errors are reported as JSON with a non-zero exit code and helpful suggestions
set +e
out=$("$BIN" edit "$W/crate.patina.json" '[{"op":"add","layer":{"channels":{"basecolour":"#fff"}}}]' --compact)
code=$?
set -e
[ $code -ne 0 ] || fail "bad edit should fail"
echo "$out" | grep -q "did you mean 'basecolor'" || fail "typo suggestion"
ok "typo -> did-you-mean error, project left untouched"

# render all modes
for mode in lit clay basecolor roughness metallic normal height ao curvature thickness bake_ao islands uv_checker mask:dust; do
  "$BIN" render "$W/crate.patina.json" --views iso --size 128 --mode "$mode" --out "$W/r_${mode/:/_}.png" --compact >/dev/null || fail "render $mode"
done
ok "render 14 modes"
"$BIN" render "$W/crate.patina.json" --sheet --size 96 --out "$W/sheet.png" --compact >/dev/null || fail "sheet"
ok "texture sheet"

# every smart material evaluates
mats=$("$BIN" library --topic smart_materials --compact | json "' '.join(d['smart_materials'].keys())")
for m in $mats; do
  "$BIN" edit "$W/crate.patina.json" "[{\"op\":\"replace\",\"layers\":[{\"id\":\"m\",\"type\":\"smart\",\"material\":\"$m\"}]}]" --compact >/dev/null || fail "smart material $m"
done
ok "all smart materials evaluate ($(echo $mats | wc -w | tr -d ' '))"

# export presets + determinism
"$BIN" new "$W/barrel.patina.json" --mesh examples/assets/barrel.glb --resolution 256 --smart rusted_metal --compact >/dev/null
for preset in blender gltf unreal unity_hdrp unity_urp godot maps; do
  "$BIN" export "$W/barrel.patina.json" --preset "$preset" --out "$W/tex_$preset" --compact >/dev/null || fail "export $preset"
  [ -f "$W/tex_$preset/manifest.json" ] || fail "manifest $preset"
done
ok "7 export presets"
"$BIN" export "$W/barrel.patina.json" --preset gltf --glb --out "$W/det1" --compact >/dev/null
"$BIN" bake "$W/barrel.patina.json" --force --compact >/dev/null
"$BIN" export "$W/barrel.patina.json" --preset gltf --glb --out "$W/det2" --compact >/dev/null
for f in "$W"/det1/*.png "$W"/det1/*.glb; do
  cmp -s "$f" "$W/det2/$(basename "$f")" || fail "non-deterministic output: $(basename "$f")"
done
ok "exports are bit-identical across runs (including a forced re-bake)"

# batch in parallel
"$BIN" new "$W/suzanne.patina.json" --mesh examples/assets/suzanne.glb --resolution 256 --smart leather --compact >/dev/null
r=$("$BIN" batch --do render "$W/crate.patina.json" "$W/barrel.patina.json" "$W/suzanne.patina.json" --size 128 -j 3 --compact | json "d['succeeded']")
[ "$r" = "3" ] || fail "batch"
ok "batch render x3"

# MCP
"$PY" tests/mcp_smoke.py "$BIN" "$W/mcp" >/dev/null || fail "mcp smoke"
ok "mcp smoke (concurrent calls)"

# install + update into a sandbox home (no PATH or MCP registration); the "release" is served over file:// URLs
EXE="$BIN"; [ -f "$EXE.exe" ] && EXE="$EXE.exe"
export PATINA_HOME="$W/home/.patina" CLAUDE_CONFIG_DIR="$W/home/.claude"
"$BIN" install --no-path --no-mcp --compact >/dev/null || fail "install"
for f in "$CLAUDE_CONFIG_DIR/skills/patina/SKILL.md" "$CLAUDE_CONFIG_DIR/skills/patina/AGENT_GUIDE.md" \
         "$PATINA_HOME/tools/blender/patina_blender.py"; do
  [ -f "$f" ] || fail "install did not write $f"
done
grep -q '^name: patina$' "$CLAUDE_CONFIG_DIR/skills/patina/SKILL.md" || fail "skill frontmatter"
ok "install (binary, skill, blender bridge)"
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) ASSET=patina-windows-x64.exe ;;
  Darwin) [ "$(uname -m)" = arm64 ] && ASSET=patina-macos-arm64 || ASSET= ;;
  *) ASSET= ;;
esac
if [ -n "$ASSET" ]; then
  rel="$W/release"; mkdir -p "$rel"; cp "$EXE" "$rel/$ASSET"
  release_json() {  # $1 = checksum to publish (defaults to the real one)
    "$PY" - "$rel" "$ASSET" "${1:-}" <<'EOF'
import hashlib, json, pathlib, sys
rel, asset, digest = pathlib.Path(sys.argv[1]).resolve(), sys.argv[2], sys.argv[3]
digest = digest or hashlib.sha256((rel / asset).read_bytes()).hexdigest()
(rel / "SHA256SUMS.txt").write_text(f"{digest}  {asset}\n")
assets = [{"name": n, "browser_download_url": (rel / n).as_uri()} for n in (asset, "SHA256SUMS.txt")]
(rel / "latest.json").write_text(json.dumps({"tag_name": "v99.0.0", "html_url": "", "assets": assets}))
print((rel / "latest.json").as_uri())
EOF
  }
  export PATINA_RELEASES_API=$(release_json)
  [ "$("$BIN" update --check --compact | json "d['update_available']")" = "True" ] || fail "update --check"
  [ "$("$BIN" update --no-path --no-mcp --compact | json "d['updated_to']")" = "99.0.0" ] || fail "update"
  export PATINA_RELEASES_API=$(release_json 0000000000000000000000000000000000000000000000000000000000000000)
  set +e; out=$("$BIN" update --no-path --no-mcp --compact); code=$?; set -e
  [ $code -ne 0 ] && echo "$out" | grep -q "checksum mismatch" || fail "bad checksum must be rejected"
  unset PATINA_RELEASES_API
  ok "update: check, verified install, checksum mismatch rejected"
fi
unset PATINA_HOME CLAUDE_CONFIG_DIR

echo "== $pass passed"
