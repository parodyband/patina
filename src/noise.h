// Seeded 3D procedural noise, evaluated at surface positions (seamless across UV seams).
#pragma once
#include "core.h"

namespace pt {

float perlin3(vec3 p, uint32_t seed);              // ~[-1, 1]
float value3(vec3 p, uint32_t seed);               // [-1, 1]
float fbm3(vec3 p, int octaves, float lacunarity, float gain, uint32_t seed);   // ~[-1, 1]
float ridged3(vec3 p, int octaves, float lacunarity, float gain, uint32_t seed);  // [0, 1]
float turbulence3(vec3 p, int octaves, float lacunarity, float gain, uint32_t seed);  // [0, 1]
struct Worley { float f1, f2; uint32_t id; };
Worley worley3(vec3 p, float jitter, uint32_t seed);
vec3 fbm3_vec(vec3 p, int octaves, uint32_t seed);  // for domain warping

}  // namespace pt
