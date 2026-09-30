// Texture export (preset-driven channel packing), manifest.json, and textured GLB output.
#pragma once
#include "eval.h"
#include "project.h"

namespace pt {

// Writes textures for all texture sets. Returns a JSON summary (files, manifest path, timings).
Json export_textures(const Project& p, const Baked& bk, const std::vector<SetResult>& results, const std::string& preset,
                     const std::string& out_dir, bool write_glb);

// Standalone GLB with embedded PBR textures (baseColor, ORM, normal, emissive).
void write_textured_glb(const std::string& path, const Mesh& m, const std::vector<SetMaps>& maps, float height_depth_world);

}  // namespace pt
