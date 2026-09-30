# Patina benchmark: stylized barrel (modelling + texturing)

You are an autonomous 3D artist working headless on Windows. Model, UV and texture a
**stylized wooden barrel** for a game, from scratch, and deliver engine-ready files.

Work only inside this directory. Do not read, copy or reference any existing barrel assets, example
projects or source code elsewhere on this machine (in particular nothing under `C:\dev\patina`).
There is no internet access. Nobody will answer questions: make sensible decisions and write them down.

## Tools

| Tool | Path | Use it for |
|---|---|---|
| Blender 5.2 (background, Python) | `C:\Program Files\Blender Foundation\Blender 5.2\blender.exe` | modelling, UVs, glTF export. Run headless: `blender.exe -b --factory-startup --python your_script.py` |
| Patina (texturing engine, CLI) | `C:\Users\AustinCrane-Work\.patina\bin\patina.exe` | inspecting the mesh, texturing, previews, export. Start with `patina.exe guide` and `patina.exe library`; every command prints JSON. |

Patina renders previews you can look at (`patina.exe render ... --out file.png`), including close-ups
(view objects with `zoom` and `target`) and debug modes (`uv_checker`, `islands`, `mask:<layer_id>`,
`curvature`). Look at your work as you go.

## The asset

A coopered barrel about **0.85 m** tall.

**Model** (Blender)
- Separate **staves** (12 to 20) following a bulged profile, with readable joints between them.
- Top and bottom **heads** built from several boards, set inside the stave ends (the staves stand
  proud of the heads at both ends).
- At least **3 iron hoops** with **rivets** on them.
- Chunky, stylized proportions. **Bevel** every hard edge: no razor-sharp edges anywhere.
- **50,000 triangles** or fewer. Standing upright on the ground (lowest point at height 0), centred
  on the vertical axis, glTF +Y up (Blender's default glTF export).
- Separate objects/materials so there are at least these texture sets: wood staves, wood heads,
  metal (hoops and rivets may share one set or have their own).

**UVs**
- No overlapping UVs within a texture set.
- Consistent texel density within each texture set.
- Wood: lay out each stave and head board so its wood grain can run along the board's length.

**Texturing** (Patina)
- A **stylized, hand-painted game look**: readable wood grain along each board, board-to-board colour
  variation, painted edge highlights driven by curvature, chunky breakup shapes, dark cavities and joints.
- **Metal that reads as metal** (not plastic, not chrome): worn edges, grime, rust where it makes sense.
- Coherent wear logic (where dirt, rust, stains and wear go and why).

## Deliverables (in this directory)

| File | Content |
|---|---|
| `barrel.glb` | the final mesh (the one the project uses) |
| `barrel.patina.json` | the Patina project; its `mesh` path is relative (`"barrel.glb"`) |
| `textures/` | `patina.exe export barrel.patina.json --preset gltf --out textures --glb` |
| `renders/hero.png` | an iso lit render at size 1024 or more; add any other renders you like |
| `NOTES.md` | what you built and why, the problems you hit and how you solved them, what you would improve |
| any scripts you wrote | e.g. `model.py` |

## How it is scored

Every submission is re-rendered by the benchmark with identical cameras and lighting, so only the
asset itself matters.

- **Hard requirements:** deliverables present, project validates, triangle budget, texture sets,
  no UV overlap, texel density spread (p95/p5) under 2 per set, height and orientation.
- **Modelling (judged):** silhouette and proportions, construction (staves, heads in the chime, hoops,
  rivets), bevels, topology efficiency.
- **UVs (judged + measured):** overlap, density, grain direction on the wood.
- **Texturing (judged):** stylization, wood, metal, wear logic, overall appeal as a game asset.
- **Process:** notes, time taken.

Aim to finish within about 90 minutes of work.
