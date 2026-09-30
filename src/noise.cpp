#include "noise.h"

namespace pt {

static inline uint32_t hash3(int x, int y, int z, uint32_t seed) {
  return hash_u32((uint32_t)x * 0x8da6b343U ^ hash_u32((uint32_t)y * 0xd8163841U ^ hash_u32((uint32_t)z * 0xcb1ab31fU ^ seed)));
}
static inline float fade(float t) { return t * t * t * (t * (t * 6.f - 15.f) + 10.f); }
static inline float grad(uint32_t h, float x, float y, float z) {
  switch (h & 15) {
    case 0: return x + y;
    case 1: return -x + y;
    case 2: return x - y;
    case 3: return -x - y;
    case 4: return x + z;
    case 5: return -x + z;
    case 6: return x - z;
    case 7: return -x - z;
    case 8: return y + z;
    case 9: return -y + z;
    case 10: return y - z;
    case 11: return -y - z;
    case 12: return x + y;
    case 13: return -y + z;
    case 14: return -x + y;
    default: return -y - z;
  }
}

float perlin3(vec3 p, uint32_t seed) {
  float fx = std::floor(p.x), fy = std::floor(p.y), fz = std::floor(p.z);
  int ix = (int)fx, iy = (int)fy, iz = (int)fz;
  float x = p.x - fx, y = p.y - fy, z = p.z - fz;
  float u = fade(x), v = fade(y), w = fade(z);
  float n000 = grad(hash3(ix, iy, iz, seed), x, y, z);
  float n100 = grad(hash3(ix + 1, iy, iz, seed), x - 1, y, z);
  float n010 = grad(hash3(ix, iy + 1, iz, seed), x, y - 1, z);
  float n110 = grad(hash3(ix + 1, iy + 1, iz, seed), x - 1, y - 1, z);
  float n001 = grad(hash3(ix, iy, iz + 1, seed), x, y, z - 1);
  float n101 = grad(hash3(ix + 1, iy, iz + 1, seed), x - 1, y, z - 1);
  float n011 = grad(hash3(ix, iy + 1, iz + 1, seed), x, y - 1, z - 1);
  float n111 = grad(hash3(ix + 1, iy + 1, iz + 1, seed), x - 1, y - 1, z - 1);
  float x00 = lerp(n000, n100, u), x10 = lerp(n010, n110, u), x01 = lerp(n001, n101, u), x11 = lerp(n011, n111, u);
  float y0 = lerp(x00, x10, v), y1 = lerp(x01, x11, v);
  return lerp(y0, y1, w) * 0.95f;
}

float value3(vec3 p, uint32_t seed) {
  float fx = std::floor(p.x), fy = std::floor(p.y), fz = std::floor(p.z);
  int ix = (int)fx, iy = (int)fy, iz = (int)fz;
  float u = fade(p.x - fx), v = fade(p.y - fy), w = fade(p.z - fz);
  auto h = [&](int a, int b, int c) { return hash_float(hash3(ix + a, iy + b, iz + c, seed)) * 2.f - 1.f; };
  float x00 = lerp(h(0, 0, 0), h(1, 0, 0), u), x10 = lerp(h(0, 1, 0), h(1, 1, 0), u);
  float x01 = lerp(h(0, 0, 1), h(1, 0, 1), u), x11 = lerp(h(0, 1, 1), h(1, 1, 1), u);
  return lerp(lerp(x00, x10, v), lerp(x01, x11, v), w);
}

// Offsets decorrelate octaves (avoid artifacts at the origin where all octaves align).
static inline vec3 octave_offset(int o, uint32_t seed) {
  uint32_t h = hash_u32(seed * 31u + (uint32_t)o * 977u);
  return {hash_float(h) * 97.f, hash_float(h ^ 0x68e31da4U) * 97.f, hash_float(h ^ 0xb5297a4dU) * 97.f};
}

float fbm3(vec3 p, int octaves, float lac, float gain, uint32_t seed) {
  float sum = 0, amp = 1, norm = 0;
  for (int o = 0; o < octaves; o++) {
    sum += perlin3(p + octave_offset(o, seed), seed + (uint32_t)o * 1013u) * amp;
    norm += amp;
    amp *= gain;
    p = p * lac;
  }
  return norm > 0 ? sum / norm : 0.f;
}

float ridged3(vec3 p, int octaves, float lac, float gain, uint32_t seed) {
  float sum = 0, amp = 1, norm = 0, weight = 1;
  for (int o = 0; o < octaves; o++) {
    float n = 1.f - std::fabs(perlin3(p + octave_offset(o, seed), seed + (uint32_t)o * 1013u));
    n *= n;
    n *= weight;
    weight = saturate(n * 2.f);
    sum += n * amp;
    norm += amp;
    amp *= gain;
    p = p * lac;
  }
  return norm > 0 ? saturate(sum / norm) : 0.f;
}

float turbulence3(vec3 p, int octaves, float lac, float gain, uint32_t seed) {
  float sum = 0, amp = 1, norm = 0;
  for (int o = 0; o < octaves; o++) {
    sum += std::fabs(perlin3(p + octave_offset(o, seed), seed + (uint32_t)o * 1013u)) * amp;
    norm += amp;
    amp *= gain;
    p = p * lac;
  }
  return norm > 0 ? saturate(sum / norm * 1.6f) : 0.f;
}

Worley worley3(vec3 p, float jitter, uint32_t seed) {
  float fx = std::floor(p.x), fy = std::floor(p.y), fz = std::floor(p.z);
  int ix = (int)fx, iy = (int)fy, iz = (int)fz;
  Worley r{1e9f, 1e9f, 0};
  for (int dz = -1; dz <= 1; dz++)
    for (int dy = -1; dy <= 1; dy++)
      for (int dx = -1; dx <= 1; dx++) {
        uint32_t h = hash3(ix + dx, iy + dy, iz + dz, seed);
        vec3 fp{(float)(ix + dx) + 0.5f + (hash_float(h) - 0.5f) * jitter, (float)(iy + dy) + 0.5f + (hash_float(h ^ 0x9e3779b9U) - 0.5f) * jitter,
                (float)(iz + dz) + 0.5f + (hash_float(h ^ 0x7f4a7c15U) - 0.5f) * jitter};
        float d = length(fp - p);
        if (d < r.f1) { r.f2 = r.f1; r.f1 = d; r.id = h; }
        else if (d < r.f2) r.f2 = d;
      }
  return r;
}

vec3 fbm3_vec(vec3 p, int octaves, uint32_t seed) {
  return {fbm3(p, octaves, 2.f, 0.5f, seed ^ 0x1234567U), fbm3(p + vec3(31.7f, 11.3f, 5.1f), octaves, 2.f, 0.5f, seed ^ 0x89abcdeU),
          fbm3(p + vec3(7.9f, 43.1f, 23.3f), octaves, 2.f, 0.5f, seed ^ 0x2468aceU)};
}

}  // namespace pt
