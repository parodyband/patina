# Patina

**A headless, agent-first texturing engine — Substance Painter for AI agents.**

Patina textures 3D assets (from Blender or anywhere that exports glTF/OBJ) with Substance-style
layer stacks: fill layers, folders, smart materials, mask stacks of generators (edge wear, dirt,
curvature, AO, gradients, noise, grunge, scratches, streaks), 3D paint strokes and projected decals.
It is a single native C++ binary for macOS and Windows with a CLI, an MCP server and a native viewer.

Built for agents, not mice:

- **Declarative & deterministic** – an asset is a JSON project file; the same file always renders the same textures.
- **Agents can see** – a built-in CPU PBR renderer returns preview images (lit, clay, per-channel, per-layer mask) in ~100 ms.
- **Fast & parallel** – baking, layer evaluation, rendering and export use every core; MCP tool calls run concurrently; `batch` runs many assets at once.
- **Seamless by construction** – procedural noise is evaluated in 3D at the surface, so there are no UV seams.
- **Engine-ready output** – export presets for Blender, glTF (ORM), Unreal, Unity HDRP/URP, Godot, plus a textured `.glb`.

## Build

Requires CMake ≥ 3.20 and a C++20 compiler (Xcode clang on macOS, MSVC 2022 on Windows). No package manager, all deps are vendored single headers.

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## Quick start

```bash
patina inspect examples/assets/crate.glb
patina new work/crate.patina.json --mesh examples/assets/crate.glb --smart painted_metal
patina render work/crate.patina.json            # -> work/renders/crate.png
patina export work/crate.patina.json --preset gltf --glb
patina view work/crate.patina.json              # native viewer, live-reloads on edits
patina mcp                                      # MCP server on stdio
```

See `patina library` for every smart material, generator, blend mode and export preset.
