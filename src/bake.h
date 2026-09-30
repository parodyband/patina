// Mesh-map baking into UV space: surface samples per texel + AO, thickness, curvature.
#pragma once
#include <memory>

#include "bvh.h"
#include "json.h"
#include "mesh.h"

namespace pt {

struct BakeSettings {
  int ao_samples = 48;
  float ao_distance = 0.3f;         // fraction of the mesh's largest dimension
  int thickness_samples = 16;
  float thickness_distance = 0.3f;  // fraction of largest dimension
  float curvature_radius = 0.012f;  // fraction of largest dimension
  float curvature_gain = 1.0f;
  float curvature_min_angle = 1.5f;  // degrees; flatter edges are ignored

  void from_json(const Json& j);
  Json to_json() const;
  uint64_t hash() const;
};

// All covered texels of one texture set, stored compactly (structure of arrays).
// "Samples" include texels whose centers are inside a triangle (interior) and gutter texels within
// ~1.5px of a triangle edge, which get the closest surface point (removes seams under filtering).
struct SampleSet {
  std::string name;
  int set = 0;
  int res = 0;
  std::vector<int32_t> texel;     // y*res + x
  std::vector<uint8_t> interior;  // 1 = texel center inside triangle
  std::vector<uint32_t> tri;
  std::vector<vec3> pos, nrm, fnrm;  // world position, smooth normal, geometric normal
  std::vector<vec4> tan;             // tangent (+U) and bitangent sign
  std::vector<vec2> uv;
  std::vector<float> len_u, len_v;   // world length of one texel step along image x / y
  std::vector<float> ao, thickness, curvature;
  std::vector<int32_t> pad;  // res*res -> nearest sample (padding / 2D neighborhood ops)
  size_t size() const { return texel.size(); }
  size_t interior_count = 0;
};

struct Baked {
  std::shared_ptr<const Mesh> mesh;
  BVH bvh;
  BakeSettings settings;
  std::vector<SampleSet> sets;  // indexed like mesh->set_names
  Json stats;
  uint64_t key = 0;
};

// res_per_set.size() == mesh->set_names.size(). cache_dir may be empty (no disk cache).
std::shared_ptr<Baked> bake_mesh(std::shared_ptr<const Mesh> mesh, const std::vector<int>& res_per_set, const BakeSettings& s,
                                 const std::string& cache_dir, bool force);

// Scatter per-sample values into a full padded image (every texel filled from its nearest sample).
void to_image(const SampleSet& ss, const float* samples, int comps, float* out_image);
// Gather image values back to samples.
void from_image(const SampleSet& ss, const float* image, int comps, float* out_samples);

}  // namespace pt
