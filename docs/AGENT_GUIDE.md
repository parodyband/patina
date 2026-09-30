# Patina agent guide

Patina textures 3D assets the way Substance Painter does (texture sets, layer stacks, masks,
generators, smart materials), but every step is a JSON edit plus a render you can look at.
This guide is everything an agent needs; `library` lists every parameter.

## 1. Mental model

| Substance Painter        | Patina                                                              |
|--------------------------|---------------------------------------------------------------------|
| Texture set              | `texture_sets.<MaterialName>` (one per material in the mesh)        |
| Bake mesh maps           | automatic + cached (AO, curvature, thickness, position, normals)    |
| Fill layer               | `{"channels": {...}, "mask": [...]}`                                 |
| Folder                   | `{"type": "folder", "layers": [...], "mask": [...]}`                 |
| Smart material           | `{"type": "smart", "material": "painted_metal", "params": {...}}`    |
| Black mask + generator   | `"mask": [{"type": "edge_wear", ...}]`                               |
| Paint / projection tool  | `paint` strokes (3D polylines) and `decal` projections               |
| Anchor point             | `{"type": "layer", "layer": "<id>"}` / `{"type": "stack", ...}`      |
| Viewport                 | `render` (returns an image), `patina view` for humans                |
| Export presets           | `export` with preset blender/gltf/unreal/unity_hdrp/unity_urp/godot  |

Layers are listed **bottom to top**: later layers paint over earlier ones.

## 2. Workflow

1. `inspect(mesh)`: read texture sets, parts (with normalized bounds), and UV warnings.
2. `new_project(project, mesh, smart="painted_metal")`: gives each texture set a starting material.
3. `edit(project, ops)`: add or tune layers. Edits are validated by evaluating before they are saved.
4. `render(project)`: look at the result. It is fast (~0.1–0.5 s), so do this after every meaningful edit.
   - Debug where a layer applies with `mode="mask:<layer_id>"`.
   - Check height work with `mode="clay"`.
   - Get close-ups with `views=[{"azimuth":30,"elevation":10,"zoom":3,"target":[0.5,0.9,0.5]}]`.
5. `variants(project, variants=[...])`: when unsure, render 2–6 alternatives in one call, then apply the best with `edit`.
6. `export(project, preset, glb=true)`: writes PNGs plus `manifest.json` (and a textured `.glb`).
7. `blender(action="apply", project=..., out="x.blend", render="x.png")`: builds Principled BSDF materials.

## 3. Coordinates and units

- **Axes (glTF):** +Y up, +Z front, +X right. Blender's +Z up becomes +Y, and Blender's front (−Y) becomes +Z.
- **Points** default to **normalized bounding-box coordinates**: `[0,0,0]` is the min corner and `[1,1,1]` the max corner.
  - `[0.5, 1, 0.5]` is the center of the top.
  - `[0.5, 0.5, 1]` is the center of the front.
  - `inspect` gives each part's bbox in these coordinates.
- **Lengths** (radius, size, falloff, depth) are fractions of the object's largest dimension.
- Set `"space": "world"` on a field to use mesh units instead.
- **Noise `scale`** is the number of features across the object (object space), so it is size-independent.
- **Directions** are `"up" | "down" | "front" | "back" | "left" | "right"` or `[x,y,z]`.

## 4. Project file

```json
{
  "patina": 1,
  "name": "crate",
  "mesh": "crate.glb",
  "resolution": 2048,
  "height_depth": 0.005,
  "bake": {"ao_samples": 48, "ao_distance": 0.3, "curvature_radius": 0.012},
  "export": {"preset": "blender", "dir": "textures/crate"},
  "texture_sets": {
    "Crate": {"resolution": 2048, "layers": [ ...bottom -> top... ]}
  }
}
```

- `height_depth`: world depth of height = 1, as a fraction of the object size. It controls normal-map strength.
- `curvature_radius` sets how wide "edges" are, for all edge generators.

## 5. Layers

**Fill layer (default type)**

```json
{"id": "paint", "name": "Paint",
 "channels": {"basecolor": "#2f5d8c", "roughness": 0.45, "metallic": 0, "height": 0.2},
 "mask": [ ...effects... ],
 "opacity": 1, "blend": "normal",
 "blend_modes": {"height": "add"}, "channel_opacity": {"roughness": 0.5},
 "enabled": true}
```

- A layer only affects the channels it lists; everything else passes through.
- Channels:

  | Channel | Values |
  |---|---|
  | `basecolor` | sRGB color |
  | `metallic` | 0..1 |
  | `roughness` | 0..1 |
  | `height` | 0 = neutral, ±values raise/lower |
  | `emissive` | color × intensity |
  | `opacity` | 0..1 |
  | `ao` | multiplier |
  | `normal` | normal-map image only |

**Channel values**

| Form | Meaning |
|---|---|
| `0.4` | constant scalar |
| `"#aa5533"`, `[0.6,0.3,0.2]` | constant sRGB color (named colors like `"rust"` or `"steel"` also work) |
| `{"type":"noise","noise":"fbm","scale":8,"range":[0.3,0.6]}` | a field remapped into a range (scalars) |
| `{"type":"noise","scale":6,"gradient":["#3a1d10","#8f4f27"]}` | a field mapped to colors (colors) |
| `{"color":"#27e36b","intensity":3}` | color with intensity (emissive) |
| `{"type":"image","path":"tex.png","projection":"triplanar","scale":4}` | tiling image; `uv`, `triplanar` or `planar` |
| `{"type":"image","path":"n.png","format":"opengl","strength":1}` | normal channel only |

**Other layer types**

- **Folder:** `{"type":"folder","layers":[...],"mask":[...]}`. The folder mask limits all of its children.
- **Smart material:**
  - `{"type":"smart","material":"rusted_metal","params":{"rust":0.7}}` expands into a folder.
  - Inner layer ids become `<id>/<inner>`, for example `mask:rust/streaks`.
  - A mask on the smart layer limits the whole material.
- **Decal:** `{"type":"decal","image":"logo.png","position":[0.5,0.6,1],"facing":"front","size":0.3,"up":"up","channels":{"roughness":0.3}}`.
  - It projects the image's colors (alpha = mask) from the facing side and is blocked by geometry closer to the projector.
  - Add `channels.basecolor` to use the image as a stencil only.

## 6. Masks

A mask is a stack of effects evaluated **bottom to top**, starting from black:

```json
"mask": [
  {"type": "dirt", "amount": 0.6},
  {"type": "grunge", "style": "smudge", "amount": 0.7, "blend": "multiply"},
  {"type": "box", "min": [0,0,0], "max": [1,0.3,1], "blend": "max", "opacity": 0.5}
]
```

- Each effect blends onto the result with `blend` (default `normal`, which replaces what is below) and `opacity`.
- Use `multiply` to intersect, `max`/`add` to union, and `subtract` to cut away.
- Patina warns when a `normal`-blended effect hides the effects below it.
- A layer with no mask covers everything.

**Generators and fields** (run `library(topic="fields")` for all parameters)

| Type | What it gives |
|------|---------------|
| `edge_wear` | worn convex edges: `amount`, `width`, `breakup` |
| `dirt` | grime in crevices: `amount` |
| `curvature` | `convex` / `concave` / `both` / `raw` |
| `ao`, `cavity`, `thickness` | baked maps |
| `gradient` | along an axis, `from`..`to` in bbox units (`"axis":"up"`) |
| `direction` | faces pointing a direction (dust/snow/moss on top) |
| `noise` | `fbm`, `perlin`, `value`, `ridged`, `turbulence`, `cells`, `voronoi`, `cracks`, `dots`, `white`, `rings` (wood rings around `axis`); 3D and seamless. `stretch` accepts `[x,y,z]` or a direction name such as `"up"` |
| `grunge` | `smudge`, `spots`, `patches`, `cracks`, `speckle`, `rust` |
| `scratches` | straight scratches: `density`, `length`, `width`, optional `direction` |
| `streaks` | vertical runs on walls (rust/water) |
| `sphere`, `box`, `plane` | 3D regions (bbox coords) with `falloff` |
| `select` | `parts` (names/globs from `inspect`) and/or UV `islands` |
| `island_random`, `part_random` | random value per UV island / part (per-plank variation) |
| `paint` | 3D brush strokes: `strokes:[{"points":[[x,y,z],...],"radius":0.03,"hardness":0.7}]` |
| `decal` | projected image alpha as a mask |
| `image` | image luminance/alpha in UV, triplanar or planar projection |
| `layer`, `stack` | reuse another layer's mask, or the accumulated channel below |
| `combine` | a nested mask stack used as one field |

**Modifiers on any field**

- They apply in this order: `blur` → `levels` [lo,hi] or {in,gamma,out} → `contrast` → `power` → `invert` → `threshold` → `multiply` → `add` → `clamp`.
- Example: `{"type":"cavity","levels":[0.1,0.5],"invert":true}`.

## 7. Recipes

**Worn painted metal with rust in crevices**

```json
[{"op":"add","set":"Body","layer":{"id":"paint","type":"smart","material":"painted_metal","params":{"color":"#3b5d2a","wear":0.5}}},
 {"op":"add","set":"Body","layer":{"id":"rust","type":"smart","material":"rust_overlay","params":{"amount":0.4}}}]
```

**Dust on top surfaces**: `{"type":"smart","material":"dust_overlay","params":{"amount":0.6}}`

**A logo on the front**

```json
{"id":"logo","type":"decal","image":"logo.png","position":[0.5,0.55,1],"facing":"front","size":0.35}
```

**Paint a stripe by hand (3D stroke, bbox coords)**

```json
{"id":"stripe","channels":{"basecolor":"#d8c21a","roughness":0.5},
 "mask":[{"type":"paint","strokes":[{"points":[[0,0.5,1],[1,0.5,1]],"radius":0.03,"hardness":0.8}]}]}
```

**Different material per part**

```json
{"id":"grip","channels":{"basecolor":"#1c1c1e","roughness":0.9},"mask":[{"type":"select","parts":["Handle*"]}]}
```

**Only the bottom 30% gets mud**

```json
"mask":[{"type":"gradient","axis":"up","from":0.3,"to":0.05,"breakup":0.3},{"type":"cavity","blend":"max","opacity":0.5}]
```

**Glowing panel light**

```json
"channels":{"emissive":{"color":"#27e36b","intensity":3}}
```

together with a `sphere` or `box` mask.

**Wear only in raised paint**: `{"type":"stack","channel":"height","levels":[0.05,0.2]}`

## 8. Look before you ship

- `render` returns an image, so check it.
  - Views: `front back left right top bottom iso iso_back iso_left iso_back_left low`, or `"az:el"`.
  - Modes:

    | Mode | Shows |
    |---|---|
    | `lit` | final look |
    | `clay` | shape / height detail |
    | `basecolor`, `roughness`, `metallic`, `normal`, `height`, `ao` | one channel |
    | `curvature`, `thickness`, `bake_ao` | bakes |
    | `mask:<id>` | where a layer applies, in red |
    | `islands`, `parts` | UV islands / mesh parts |
    | `uv_checker` | UV distortion |

  - `sheet=true` shows the flat texture maps.
- Iterate cheaply: previews render at `min(1024, resolution)` and bakes are cached per mesh and settings.
- Check that metals read as metal (metallic 1 with a mid-to-low roughness), paint is dielectric (metallic 0), and nothing is pure black or white in basecolor.

## 9. Parallel work

- MCP tool calls run concurrently. Texture many assets at once, one project each.
- `batch(jobs=[{"command":"export","args":{"project":"a.patina.json"}}, ...], parallel=8)`.
  - CLI equivalent: `patina batch --do export a.patina.json b.patina.json -j 8`.
- `variants` evaluates alternatives in parallel and returns one comparison image.
- Projects are independent files, so several agents can each own one; edits to one project are serialized.

## 10. Export presets

| Preset       | Files                                                                          | Normal  |
|--------------|--------------------------------------------------------------------------------|---------|
| `blender`    | BaseColor, Metallic, Roughness, Normal, Height (16-bit), AO, [Emissive, Opacity] | OpenGL  |
| `gltf`       | BaseColor(+A), ORM (R=AO G=Rough B=Metal), Normal, [Emissive]; `glb=true`      | OpenGL  |
| `unreal`     | T_BC, T_ORM, T_N, [T_E]                                                        | DirectX |
| `unity_hdrp` | BaseMap, MaskMap (R=Metal G=AO B=Detail A=Smooth), Normal                      | OpenGL  |
| `unity_urp`  | BaseMap, MetallicSmoothness, Occlusion, Normal                                 | OpenGL  |
| `godot`      | albedo, orm, normal                                                            | OpenGL  |
| `maps`       | every channel and bake as separate files                                       | OpenGL  |

- `manifest.json` lists every file per texture set. `blender(action="apply")` reads it.
- The normal map already contains the height detail.

## 11. Custom smart materials

Put JSON files in `<project dir>/library/` or in a directory on `$PATINA_LIBRARY`:

```json
{"smart_materials": {"my_paint": {
  "description": "...",
  "params": {"color": {"default": "#aa3322", "doc": "paint color"}, "wear": {"default": 0.4}},
  "layers": [{"id": "p", "channels": {"basecolor": "${color}"}, "mask": [{"type": "edge_wear", "amount": "${wear}", "invert": true}]}]
}}}
```

- `"${param}"` substitutes a parameter value.
- `"${wear*0.5+0.1}"` evaluates arithmetic.
- `"extends": "other_material"` inherits another material's layers with new defaults.
