// Image-based lighting: equirectangular HDR environments, prefiltered with GGX importance sampling
// ("filtered importance sampling": each sample reads a pyramid level matching its solid angle, so a
// few hundred samples per texel give smooth, firefly-free results). Built once per environment.
#include "envmap.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <mutex>

#include "stb_image.h"

namespace pt {

namespace {

const unsigned char kStudioHdri[] = {
#include "patina_studio_hdri.inc"
};

struct Level {
  int w = 0, h = 0;
  std::vector<vec3> px;
};

// equirect: u = azimuth (atan2(x, -z)), v = polar angle from +Y
inline vec2 dir_to_uv(vec3 d) {
  float u = std::atan2(d.x, -d.z) * (0.5f / kPi) + 0.5f;
  float v = std::acos(clampf(d.y, -1.f, 1.f)) / kPi;
  return {u, v};
}

inline vec3 uv_to_dir(float u, float v) {
  float phi = (u - 0.5f) * 2.f * kPi, theta = v * kPi;
  float st = std::sin(theta);
  return {st * std::sin(phi), std::cos(theta), -st * std::cos(phi)};
}

inline vec3 sample_level(const vec3* px, int w, int h, vec3 d) {
  vec2 uv = dir_to_uv(d);
  float x = uv.x * w - 0.5f, y = uv.y * h - 0.5f;
  float fx = std::floor(x), fy = std::floor(y);
  float tx = x - fx, ty = y - fy;
  int x0 = ((int)fx % w + w) % w, x1 = (x0 + 1) % w;
  int y0 = std::clamp((int)fy, 0, h - 1), y1 = std::clamp((int)fy + 1, 0, h - 1);
  vec3 a = px[(size_t)y0 * w + x0], b = px[(size_t)y0 * w + x1], c = px[(size_t)y1 * w + x0], e = px[(size_t)y1 * w + x1];
  return lerp(lerp(a, b, tx), lerp(c, e, tx), ty);
}

inline vec3 sample_pyramid(const std::vector<Level>& pyr, float lod, vec3 d) {
  lod = clampf(lod, 0.f, (float)pyr.size() - 1.f);
  int l0 = (int)lod, l1 = std::min(l0 + 1, (int)pyr.size() - 1);
  vec3 a = sample_level(pyr[l0].px.data(), pyr[l0].w, pyr[l0].h, d);
  if (l1 == l0) return a;
  return lerp(a, sample_level(pyr[l1].px.data(), pyr[l1].w, pyr[l1].h, d), lod - l0);
}

Level downsample(const Level& s) {
  Level d;
  d.w = std::max(1, s.w / 2);
  d.h = std::max(1, s.h / 2);
  d.px.resize((size_t)d.w * d.h);
  parallel_for(d.h, 1, [&](int64_t y0, int64_t y1) {
    for (int64_t y = y0; y < y1; y++)
      for (int x = 0; x < d.w; x++) {
        int sx = std::min(x * 2, s.w - 1), sy = std::min((int)y * 2, s.h - 1);
        int sx1 = std::min(sx + 1, s.w - 1), sy1 = std::min(sy + 1, s.h - 1);
        d.px[(size_t)y * d.w + x] = (s.px[(size_t)sy * s.w + sx] + s.px[(size_t)sy * s.w + sx1] + s.px[(size_t)sy1 * s.w + sx] +
                                     s.px[(size_t)sy1 * s.w + sx1]) * 0.25f;
      }
  });
  return d;
}

inline vec2 hammersley(uint32_t i, uint32_t n) {
  uint32_t b = i;
  b = (b << 16u) | (b >> 16u);
  b = ((b & 0x55555555u) << 1u) | ((b & 0xAAAAAAAAu) >> 1u);
  b = ((b & 0x33333333u) << 2u) | ((b & 0xCCCCCCCCu) >> 2u);
  b = ((b & 0x0F0F0F0Fu) << 4u) | ((b & 0xF0F0F0F0u) >> 4u);
  b = ((b & 0x00FF00FFu) << 8u) | ((b & 0xFF00FF00u) >> 8u);
  return {(float)i / n, (float)b * 2.3283064365386963e-10f};
}

std::shared_ptr<EnvMap> build(const float* rgb, int w, int h, const std::string& name) {
  // source pyramid (clamped: a few extreme texels would otherwise dominate the rough levels)
  std::vector<Level> pyr(1);
  pyr[0].w = w;
  pyr[0].h = h;
  pyr[0].px.resize((size_t)w * h);
  for (size_t i = 0; i < pyr[0].px.size(); i++)
    pyr[0].px[i] = vmin(vec3(rgb[i * 3], rgb[i * 3 + 1], rgb[i * 3 + 2]), vec3(500.f));
  while (pyr.back().h > 4) pyr.push_back(downsample(pyr.back()));
  const float texel_sa = 4.f * kPi / ((float)w * h);

  auto E = std::make_shared<EnvMap>();
  E->name = name;
  for (int k = 0; k < EnvMap::kLevels; k++) {
    float rough = (float)k / (EnvMap::kLevels - 1);
    int ow = k == 0 ? std::min(w, 512) : std::max(32, 512 >> k);
    int oh = ow / 2;
    E->w[k] = ow;
    E->h[k] = oh;
    E->spec[k].resize((size_t)ow * oh);
    if (k == 0) {
      size_t l = 0;
      while (pyr[l].w > ow) l++;
      E->spec[0] = pyr[l].px;
      E->w[0] = pyr[l].w;
      E->h[0] = pyr[l].h;
      continue;
    }
    float a = rough * rough, a2 = a * a;
    const uint32_t M = 192;
    parallel_for(oh, 1, [&](int64_t y0, int64_t y1) {
      for (int64_t y = y0; y < y1; y++)
        for (int x = 0; x < ow; x++) {
          vec3 N = uv_to_dir((x + 0.5f) / ow, (y + 0.5f) / oh), T, B;
          onb(N, T, B);
          vec3 sum{0, 0, 0};
          float wsum = 0;
          for (uint32_t i = 0; i < M; i++) {
            vec2 xi = hammersley(i, M);
            float phi = 2.f * kPi * xi.x;
            float ct = std::sqrt((1.f - xi.y) / (1.f + (a2 - 1.f) * xi.y)), st = std::sqrt(std::fmax(0.f, 1.f - ct * ct));
            vec3 H = T * (st * std::cos(phi)) + B * (st * std::sin(phi)) + N * ct;
            vec3 L = H * (2.f * dot(N, H)) - N;  // V = N
            float NdotL = dot(N, L);
            if (NdotL <= 0) continue;
            float d = ct * ct * (a2 - 1.f) + 1.f;
            float D = a2 / (kPi * d * d);
            float pdf = D * 0.25f;  // D * NdotH / (4 VdotH) with V = N
            float lod = 0.5f * std::log2((1.f / (M * pdf + 1e-6f)) / texel_sa) + 1.f;
            sum += sample_pyramid(pyr, lod, L) * NdotL;
            wsum += NdotL;
          }
          E->spec[k][(size_t)y * ow + x] = wsum > 0 ? sum / wsum : vec3(0.f);
        }
    });
  }
  // diffuse: mean radiance over the cosine lobe (= irradiance / pi)
  E->dw = 32;
  E->dh = 16;
  E->diffuse.resize((size_t)E->dw * E->dh);
  parallel_for(E->dh, 1, [&](int64_t y0, int64_t y1) {
    const uint32_t M = 512;
    for (int64_t y = y0; y < y1; y++)
      for (int x = 0; x < E->dw; x++) {
        vec3 N = uv_to_dir((x + 0.5f) / E->dw, (y + 0.5f) / E->dh), T, B;
        onb(N, T, B);
        vec3 sum{0, 0, 0};
        for (uint32_t i = 0; i < M; i++) {
          vec2 xi = hammersley(i, M);
          float r = std::sqrt(xi.y), phi = 2.f * kPi * xi.x;
          float ct = std::sqrt(std::fmax(0.f, 1.f - xi.y));
          vec3 L = T * (r * std::cos(phi)) + B * (r * std::sin(phi)) + N * ct;
          float lod = 0.5f * std::log2((1.f / (M * ct / kPi + 1e-6f)) / texel_sa) + 1.f;
          sum += sample_pyramid(pyr, lod, L);
        }
        E->diffuse[(size_t)y * E->dw + x] = sum / (float)M;
      }
  });
  // normalize brightness: environments differ by orders of magnitude, previews should not
  double lum = 0, wsum = 0;
  for (int y = 0; y < E->dh; y++) {
    float st = std::sin((y + 0.5f) / E->dh * kPi);
    for (int x = 0; x < E->dw; x++) {
      vec3 c = E->diffuse[(size_t)y * E->dw + x];
      lum += (0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z) * st;
      wsum += st;
    }
  }
  float k = lum > 0 ? (float)(0.5 / (lum / wsum)) : 1.f;
  for (auto& c : E->diffuse) c *= k;
  for (auto& lv : E->spec)
    for (auto& c : lv) c *= k;
  return E;
}

}  // namespace

vec3 EnvMap::radiance(vec3 d, float rough) const {
  float f = saturate(rough) * (kLevels - 1);
  int k0 = std::min((int)f, kLevels - 1), k1 = std::min(k0 + 1, kLevels - 1);
  vec3 a = sample_level(spec[k0].data(), w[k0], h[k0], d);
  if (k1 == k0) return a;
  return lerp(a, sample_level(spec[k1].data(), w[k1], h[k1], d), f - k0);
}

vec3 EnvMap::irradiance(vec3 n) const { return sample_level(diffuse.data(), dw, dh, n); }

std::shared_ptr<const EnvMap> load_environment(const std::string& spec) {
  static std::mutex m;
  static std::map<std::string, std::shared_ptr<const EnvMap>> cache;
  std::string key = spec == "studio" ? spec : strf("%s@%lld", path_abs(spec).c_str(), (long long)file_mtime_ns(spec));
  std::lock_guard<std::mutex> lk(m);
  if (auto it = cache.find(key); it != cache.end()) return it->second;
  int w = 0, h = 0, comps = 0;
  float* px = nullptr;
  if (spec == "studio") {
    px = stbi_loadf_from_memory(kStudioHdri, (int)sizeof(kStudioHdri) - 1, &w, &h, &comps, 3);
  } else {
    if (!file_exists(spec)) fail("environment '%s' not found (use \"studio\", \"procedural\" or a path to an equirectangular .hdr)", spec.c_str());
    px = stbi_loadf(spec.c_str(), &w, &h, &comps, 3);
  }
  if (!px) fail("cannot load environment '%s': %s", spec.c_str(), stbi_failure_reason());
  std::shared_ptr<const EnvMap> e = build(px, w, h, spec);
  stbi_image_free(px);
  cache[key] = e;
  return e;
}

}  // namespace pt
