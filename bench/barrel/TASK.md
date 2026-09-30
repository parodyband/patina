# Patina benchmark: stylized beer barrel (modelling + texturing)

You are an autonomous 3D artist working headless on Windows. Model, UV and texture a **stylized
beer barrel set** for a game, from scratch, and deliver engine-ready files.

Work only inside this directory. Do not read, copy or reference any existing barrel assets, example
projects or source code elsewhere on this machine (in particular nothing under `C:\dev\patina`).
There is no internet access. Nobody will answer questions: make sensible decisions and write them down.

## Tools

| Tool | Path | Use it for |
|---|---|---|
| Blender 5.2 (background, Python) | `C:\Program Files\Blender Foundation\Blender 5.2\blender.exe` | modelling, UVs, glTF export. Run headless: `blender.exe -b --factory-startup --python your_script.py` |
| Patina (texturing engine, CLI) | `C:\Users\AustinCrane-Work\.patina\bin\patina.exe` | inspecting the mesh, texturing, previews, export. Start with `patina.exe guide` and `patina.exe library`; every command prints JSON. |

Patina renders previews you can look at (`patina.exe render ... --out file.png`), including close-ups
(view objects with `zoom` and `target`), debug modes (`wireframe`, `uv_checker`, `islands`,
`mask:<layer_id>`, `curvature`) and flat UV layouts (`--uv-layout`); `inspect` reports UV overlap and
texel density per texture set. Look at your work as you go.

## The asset

A coopered **beer barrel lying on its side in a wooden holder**, with a tap. The barrel is about
**0.85 m** long; the whole set stands on the ground.

**Model** (Blender)
- **Barrel**
  - separate **staves** (12 to 20) following a bulged profile, with readable joints between them;
  - front and back **heads** built from several boards, set inside the stave ends (the staves stand
    proud of the heads at both ends);
  - at least **3 iron hoops** with **rivets** on them;
  - a **bung hole** in the top stave with a wooden **bung** (plug) in it.
- **Tap**: a brass tap (spigot) in the lower part of the front head, with a spout pointing down and a
  handle or lever.
- **Holder**: a wooden cradle the barrel rests in, for example two saddles cut to the barrel's curve,
  joined by rails. The barrel must sit in it convincingly (no floating, no deep intersection).
- Chunky, stylized proportions. **Bevel** every hard edge: no razor-sharp edges anywhere.
- **60,000 triangles** or fewer for the whole set. Lowest point at height 0, centred on the vertical
  axis, the barrel's axis running front to back (tap facing the front, glTF +Z), glTF +Y up
  (Blender's default glTF export).
- Separate objects/materials so there are at least these texture sets: barrel staves, barrel heads,
  metal (hoops and rivets may share one set or have their own), tap (brass), holder (wood).

**UVs**
- No overlapping UVs within a texture set.
- Consistent texel density within each texture set.
- Wood: lay out each stave, head board and holder plank so its wood grain can run along its length.

**Texturing** (Patina)
- A **stylized, hand-painted game look**: readable wood grain along each board, board-to-board colour
  variation, painted edge highlights driven by curvature, chunky breakup shapes, dark cavities and joints.
- **Metal that reads as metal** (not plastic, not chrome): iron hoops and rivets with worn edges, grime
  and rust; a brass tap with its own character.
- **Beer story**: wet, darker, glossier wood and drips below the tap, a spill mark on the holder, and a
  brand burned or painted on the front head.
- Coherent wear logic (where dirt, rust, stains and wear go and why).

## Deliverables (in this directory)

| File | Content |
|---|---|
| `barrel.glb` | the final mesh of the whole set (the one the project uses) |
| `barrel.patina.json` | the Patina project; its `mesh` path is relative (`"barrel.glb"`) |
| `textures/` | `patina.exe export barrel.patina.json --preset gltf --out textures --glb` |
| `renders/hero.png` | an iso lit render at size 1024 or more; add any other renders you like |
| `NOTES.md` | what you built and why, the problems you hit and how you solved them, what you would improve |
| any scripts you wrote | e.g. `model.py` |

## How it is scored

Every submission is re-rendered by the benchmark with identical cameras and lighting, so only the
asset itself matters.

- Scoring renders include wireframes (topology) and UV layouts of every texture set.
- **Hard requirements:** deliverables present, project validates, triangle budget, texture sets,
  no UV overlap, texel density spread (p95/p5) under 2 per set, size and placement.
- **Modelling (judged):** silhouette and proportions, construction (staves, heads in the chime, hoops,
  rivets, bung, tap, holder and how the barrel sits in it), bevels, topology efficiency.
- **UVs (judged + measured):** overlap, density, grain direction on the wood.
- **Texturing (judged):** stylization, wood, iron, brass, the beer story, wear logic, overall appeal
  as a game asset.
- **Process:** notes, time taken.

Aim to finish within about 90 minutes of work.
