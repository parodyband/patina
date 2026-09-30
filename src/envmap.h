// Image-based lighting from equirectangular HDR environments (.hdr), prefiltered for GGX.
#pragma once
#include <memory>
#include <string>
#include <vector>

#include "core.h"

namespace pt {

struct EnvMap {
  // Radiance prefiltered for roughness 0, 0.2, ... 1.0 (equirect, +Y up), plus cosine irradiance.
  static constexpr int kLevels = 6;
  int w[kLevels] = {}, h[kLevels] = {};
  std::vector<vec3> spec[kLevels];
  int dw = 0, dh = 0;
  std::vector<vec3> diffuse;  // irradiance / pi (multiply by albedo)
  std::string name;

  vec3 radiance(vec3 dir, float rough) const;  // world direction, reflected
  vec3 irradiance(vec3 n) const;               // world normal
};

// "studio" = the built-in HDRI (third_party/hdri); otherwise a path to an equirectangular .hdr.
// Normalized to a common brightness, prefiltered once and cached. Throws pt::Error.
std::shared_ptr<const EnvMap> load_environment(const std::string& spec);

}  // namespace pt
