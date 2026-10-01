# Patina benchmark: match the reference (stylized beer barrel, game-ready)

You are an autonomous 3D environment artist working headless on Windows. Recreate the prop in
`reference.png` as a **game-ready asset**, as close to the reference as you can, following game-art
best practice: sculpted high poly, clean low poly, one material with maximized UVs, high-to-low bake,
stylized texturing.

Work only inside this directory. Do not read, copy or reference any existing barrel assets, example
projects or source code elsewhere on this machine (in particular nothing under `C:\dev\patina` or
`C:\dev\patina-bench` outside this directory). There is no internet access. Nobody will answer
questions: make sensible decisions and write them down.

## Tools and guidance

| Tool | Path | Use it for |
|---|---|---|
| Blender 5.2 (background, Python) | `C:\Program Files\Blender Foundation\Blender 5.2\blender.exe` | high poly (including sculpting), low poly, UVs, glTF export. Headless only: `blender.exe -b --factory-startup --python your_script.py` |
| Patina (texturing engine, CLI) | `C:\Users\AustinCrane-Work\.patina\bin\patina.exe` | baking (high to low normals, AO, curvature), texturing, previews, export. Start with `patina.exe guide` (section 12 is baking) and `patina.exe library`; every command prints JSON. |
| Skill: stylized environment art | `skills/stylized-environment-art/SKILL.md` in this directory (also installed as a skill) | stylized game-art decisions: high/low poly, baking, UVs, texel density, texturing, QA. Read it before you start. |

Patina renders previews you can look at, including close-ups (view objects with `zoom` and `target`),
`wireframe`, `uv_checker`, `bake_normal`, `bake_misses`, `mask:<layer_id>` and flat UV layouts
(`--uv-layout`). `patina.exe inspect` reports UV overlap, texel density spread and sliver triangles.
Look at your work against `reference.png` as you go.

## The asset: match `reference.png`

A chunky stylized wooden barrel lying on its side on a wooden trestle stand: wide staves with
heavily rounded, chipped edges, dark iron hoops with square bolt heads, a front head of vertical boards,
a wooden tap with an iron collar in the front head, a wooden bung on top, and a stand of splayed legs
joined by a beam, with square bolt heads on the legs. Match its silhouette, proportions, part
count and placement, shapes, palette and painted look. About 0.9 m long; it stands on the ground.

Orientation: barrel axis front to back with the tap facing the front (glTF +Z), glTF +Y up (Blender's
default glTF export), lowest point at height 0, centred on the vertical axis.

## Game-ready requirements

1. **High poly (sculpted, Blender headless).** Build `barrel_high.glb`: the same parts with the chunky
   rounded, chipped and dented edges and carved detail the reference shows, made with Blender's
   modelling and sculpting tools from a script (bevels, subdivision, remesh, displacement, sculpt
   operations; no hand interaction). Much denser than the low poly.
2. **Low poly (game topology).** Build `barrel_low.glb`: **8,000 triangles or fewer** (a typical
   stylized prop of this size is 2,000–6,000). Clean, evenly distributed, quad-based topology that
   holds the silhouette: no fan-filled n-gons, no long thin slivers, no wasted hidden faces where you
   can avoid them, hard edges only on UV seams. The low poly carries the silhouette; the high poly's
   edges and detail come through the bake.
3. **One material, maximized UVs.** All parts share **one material and one texture set** (2048²).
   One UV layout, packed for maximum coverage: no overlaps (no mirrored or stacked islands; the normal
   bake needs unique space), consistent texel density, enough padding between islands for mipmaps,
   wood islands laid out so the grain runs along each board.
4. **Bake.** Bake the high poly onto the low poly with Patina: in `barrel.patina.json`, set
   `bake.normal.high` to `barrel_high.glb`. Name parts so they match for the bake (`Part_low` gets
   baked only from `Part_high`). Check `bake_normal` and `bake_misses` and fix misses and skew.
5. **Texture (Patina).** A stylized, hand-painted look matching the reference: orange-amber staves,
   darker brown stand, dark iron hoops and bolts, painted edge highlights, chunky breakup, dark
   cavities.

## Deliverables (in this directory)

| File | Content |
|---|---|
| `barrel_low.glb` | the game mesh (the project's `mesh`, relative path) |
| `barrel_high.glb` | the sculpted high poly the project bakes from |
| `barrel.patina.json` | the Patina project: `"mesh": "barrel_low.glb"`, `bake.normal.high` = `"barrel_high.glb"` |
| `textures/` | `patina.exe export barrel.patina.json --preset gltf --out textures --glb` |
| `renders/hero.png` | a lit render from the reference angle: `--views "[{\"azimuth\":55,\"elevation\":15}]" --size 1254` |
| `NOTES.md` | what you built and why, your budgets and topology/UV decisions, the problems you hit and how you solved them, what you would improve |
| any scripts you wrote | e.g. `highpoly.py`, `lowpoly.py`, `texture.py` |

## How it is scored

Every submission is re-baked and re-rendered by the benchmark with identical cameras and lighting
(the reference angle included), next to the reference.

- **Hard requirements:**
  - deliverables present and the project validates;
  - low poly 8,000 triangles or fewer, and the high poly at least 5 times denser;
  - exactly one texture set;
  - UV coverage of at least 60%, UV overlap under 0.5%, texel density spread (p95/p5) under 1.6;
  - sliver triangles under 15% of the low poly;
  - bake misses under 2%;
  - size and placement.
- **Likeness to the reference (judged, the biggest weight):** silhouette, proportions, parts, palette,
  stylization.
- **Game readiness (judged + measured):** topology, UV packing, bake quality, budget use.
- **Texturing (judged).**
- **Process:** notes, time taken.

Aim to finish within about 90 minutes of work.
