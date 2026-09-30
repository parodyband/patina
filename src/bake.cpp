#include "bake.h"

#include <algorithm>
#include <cstdio>
#include <unordered_map>

namespace pt {

// ---------------------------------------------------------------- settings
void BakeSettings::from_json(const Json& j) {
  if (!j.is_object()) return;
  ao_samples = std::clamp(j.integer("ao_samples", ao_samples), 4, 1024);
  ao_distance = std::clamp(j.numf("ao_distance", ao_distance), 1e-4f, 10.f);
  thickness_samples = std::clamp(j.integer("thickness_samples", thickness_samples), 4, 512);
  thickness_distance = std::clamp(j.numf("thickness_distance", thickness_distance), 1e-4f, 10.f);
  curvature_radius = std::clamp(j.numf("curvature_radius", curvature_radius), 1e-4f, 0.5f);
  curvature_gain = std::clamp(j.numf("curvature_gain", curvature_gain), 0.01f, 100.f);
  curvature_min_angle = std::clamp(j.numf("curvature_min_angle", curvature_min_angle), 0.f, 90.f);
}
Json BakeSettings::to_json() const {
  Json j = Json::object();
  j.set("ao_samples", ao_samples);
  j.set("ao_distance", ao_distance);
  j.set("thickness_samples", thickness_samples);
  j.set("thickness_distance", thickness_distance);
  j.set("curvature_radius", curvature_radius);
  j.set("curvature_gain", curvature_gain);
  j.set("curvature_min_angle", curvature_min_angle);
  return j;
}
uint64_t BakeSettings::hash() const { return fnv1a(to_json().dump()); }

// ---------------------------------------------------------------- UV rasterization
namespace {
constexpr int kTile = 32;
constexpr float kGutter = 1.5f;  // texels around triangles that receive closest-point samples

struct Hit {
  int32_t tri;
  float w0, w1, w2;  // barycentric weights of vertices 0,1,2
  float dist;        // 0 = inside
};

inline float edge_fn(vec2 a, vec2 b, vec2 p) { return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x); }

// Closest point on triangle (2D) to p, returned as barycentrics + distance.
inline float closest_on_tri(vec2 a, vec2 b, vec2 c, vec2 p, float& w0, float& w1, float& w2) {
  float best = 1e30f;
  auto seg = [&](vec2 A, vec2 B, int ia, int ib) {
    vec2 ab = B - A;
    float l2 = ab.x * ab.x + ab.y * ab.y;
    float t = l2 > 0 ? clampf(((p.x - A.x) * ab.x + (p.y - A.y) * ab.y) / l2, 0.f, 1.f) : 0.f;
    vec2 q = A + ab * t;
    float d = std::sqrt((p.x - q.x) * (p.x - q.x) + (p.y - q.y) * (p.y - q.y));
    if (d < best) {
      best = d;
      float w[3] = {0, 0, 0};
      w[ia] = 1.f - t;
      w[ib] = t;
      w0 = w[0]; w1 = w[1]; w2 = w[2];
    }
  };
  seg(a, b, 0, 1);
  seg(b, c, 1, 2);
  seg(c, a, 2, 0);
  return best;
}
}  // namespace

static void raster_set(const Mesh& m, int set, int res, SampleSet& ss) {
  int tiles = (res + kTile - 1) / kTile;
  std::vector<std::vector<uint32_t>> bins((size_t)tiles * tiles);
  float fres = (float)res;
  for (size_t t = 0; t < m.tri_count(); t++) {
    if (m.tri_set[t] != set) continue;
    vec2 a = m.uv[m.idx[t * 3]] * fres, b = m.uv[m.idx[t * 3 + 1]] * fres, c = m.uv[m.idx[t * 3 + 2]] * fres;
    if (std::fabs(edge_fn(a, b, c)) < 1e-9f) continue;
    float x0 = std::fmin(a.x, std::fmin(b.x, c.x)) - kGutter, x1 = std::fmax(a.x, std::fmax(b.x, c.x)) + kGutter;
    float y0 = std::fmin(a.y, std::fmin(b.y, c.y)) - kGutter, y1 = std::fmax(a.y, std::fmax(b.y, c.y)) + kGutter;
    int tx0 = std::max(0, (int)std::floor(x0) / kTile), tx1 = std::min(tiles - 1, (int)std::floor(x1) / kTile);
    int ty0 = std::max(0, (int)std::floor(y0) / kTile), ty1 = std::min(tiles - 1, (int)std::floor(y1) / kTile);
    if (x1 < 0 || y1 < 0 || x0 >= fres || y0 >= fres) continue;
    for (int ty = ty0; ty <= ty1; ty++)
      for (int tx = tx0; tx <= tx1; tx++) bins[(size_t)ty * tiles + tx].push_back((uint32_t)t);
  }

  std::vector<std::vector<Hit>> tile_hits((size_t)tiles * tiles);
  std::vector<std::vector<int32_t>> tile_texels((size_t)tiles * tiles);
  parallel_for((int64_t)tiles * tiles, 1, [&](int64_t b0, int64_t b1) {
    std::vector<Hit> local(kTile * kTile);
    for (int64_t ti = b0; ti < b1; ti++) {
      auto& bin = bins[ti];
      if (bin.empty()) continue;
      int tx = (int)(ti % tiles), ty = (int)(ti / tiles);
      int px0 = tx * kTile, py0 = ty * kTile;
      for (auto& h : local) { h.tri = -1; h.dist = 1e30f; }
      for (uint32_t t : bin) {
        vec2 a = m.uv[m.idx[t * 3]] * fres, b = m.uv[m.idx[t * 3 + 1]] * fres, c = m.uv[m.idx[t * 3 + 2]] * fres;
        float area = edge_fn(a, b, c);
        float inv_area = 1.0f / area;
        int x0 = std::max(px0, (int)std::floor(std::fmin(a.x, std::fmin(b.x, c.x)) - kGutter));
        int x1 = std::min(std::min(px0 + kTile, res) - 1, (int)std::floor(std::fmax(a.x, std::fmax(b.x, c.x)) + kGutter));
        int y0 = std::max(py0, (int)std::floor(std::fmin(a.y, std::fmin(b.y, c.y)) - kGutter));
        int y1 = std::min(std::min(py0 + kTile, res) - 1, (int)std::floor(std::fmax(a.y, std::fmax(b.y, c.y)) + kGutter));
        for (int y = y0; y <= y1; y++)
          for (int x = x0; x <= x1; x++) {
            vec2 p{x + 0.5f, y + 0.5f};
            float w0 = edge_fn(b, c, p) * inv_area, w1 = edge_fn(c, a, p) * inv_area, w2 = 1.f - w0 - w1;
            Hit& h = local[(y - py0) * kTile + (x - px0)];
            if (w0 >= -1e-6f && w1 >= -1e-6f && w2 >= -1e-6f) {
              if (h.dist > 0.f) h = {(int32_t)t, w0, w1, w2, 0.f};
            } else if (h.dist > 0.f) {
              float c0, c1, c2;
              float d = closest_on_tri(a, b, c, p, c0, c1, c2);
              if (d < kGutter && d < h.dist) h = {(int32_t)t, c0, c1, c2, d};
            }
          }
      }
      auto& out = tile_hits[ti];
      auto& tex = tile_texels[ti];
      for (int ly = 0; ly < kTile; ly++)
        for (int lx = 0; lx < kTile; lx++) {
          const Hit& h = local[ly * kTile + lx];
          if (h.tri < 0) continue;
          int x = px0 + lx, y = py0 + ly;
          if (x >= res || y >= res) continue;
          out.push_back(h);
          tex.push_back(y * res + x);
        }
    }
  });

  size_t total = 0;
  std::vector<size_t> offset(tile_hits.size());
  for (size_t i = 0; i < tile_hits.size(); i++) { offset[i] = total; total += tile_hits[i].size(); }
  ss.texel.resize(total);
  ss.interior.resize(total);
  ss.tri.resize(total);
  ss.pos.resize(total);
  ss.nrm.resize(total);
  ss.fnrm.resize(total);
  ss.tan.resize(total);
  ss.uv.resize(total);
  ss.len_u.resize(total);
  ss.len_v.resize(total);

  parallel_for((int64_t)tile_hits.size(), 4, [&](int64_t b0, int64_t b1) {
    for (int64_t ti = b0; ti < b1; ti++) {
      size_t o = offset[ti];
      for (size_t k = 0; k < tile_hits[ti].size(); k++) {
        const Hit& h = tile_hits[ti][k];
        size_t i = o + k;
        uint32_t t = (uint32_t)h.tri;
        uint32_t i0 = m.idx[t * 3], i1 = m.idx[t * 3 + 1], i2 = m.idx[t * 3 + 2];
        ss.texel[i] = tile_texels[ti][k];
        ss.interior[i] = h.dist == 0.f;
        ss.tri[i] = t;
        ss.pos[i] = m.pos[i0] * h.w0 + m.pos[i1] * h.w1 + m.pos[i2] * h.w2;
        vec3 fn = cross(m.pos[i1] - m.pos[i0], m.pos[i2] - m.pos[i0]);
        vec3 n = normalize(m.nrm[i0] * h.w0 + m.nrm[i1] * h.w1 + m.nrm[i2] * h.w2);
        fn = normalize(fn);
        if (dot(fn, n) < 0) fn = -fn;
        ss.nrm[i] = n;
        ss.fnrm[i] = fn;
        vec3 tg = m.tan[i0].xyz() * h.w0 + m.tan[i1].xyz() * h.w1 + m.tan[i2].xyz() * h.w2;
        tg = normalize(tg - n * dot(n, tg));
        ss.tan[i] = vec4(tg, m.tan[i0].w);
        ss.uv[i] = m.uv[i0] * h.w0 + m.uv[i1] * h.w1 + m.uv[i2] * h.w2;
        // world units per UV unit along u and v (glTF v, pointing down the image)
        vec3 e1 = m.pos[i1] - m.pos[i0], e2 = m.pos[i2] - m.pos[i0];
        float du1 = m.uv[i1].x - m.uv[i0].x, dv1 = m.uv[i1].y - m.uv[i0].y;
        float du2 = m.uv[i2].x - m.uv[i0].x, dv2 = m.uv[i2].y - m.uv[i0].y;
        float det = du1 * dv2 - du2 * dv1;
        if (std::fabs(det) > 1e-20f) {
          vec3 dpdu = (e1 * dv2 - e2 * dv1) / det;
          vec3 dpdv = (e2 * du1 - e1 * du2) / det;
          ss.len_u[i] = std::fmax(1e-9f, length(dpdu) / res);
          ss.len_v[i] = std::fmax(1e-9f, length(dpdv) / res);
        } else {
          ss.len_u[i] = ss.len_v[i] = 1e-3f;
        }
      }
    }
  });
  size_t interior = 0;
  for (auto v : ss.interior) interior += v;
  ss.interior_count = interior;
}

// Nearest-sample map via multi-source BFS (8-connected): pads every texel with the value of the
// closest covered texel so filtering/mipmapping never bleeds background into islands.
static void build_padding(SampleSet& ss) {
  int res = ss.res;
  ss.pad.assign((size_t)res * res, -1);
  std::vector<int32_t> queue;
  queue.reserve((size_t)res * res);
  for (size_t i = 0; i < ss.size(); i++) {
    ss.pad[ss.texel[i]] = (int32_t)i;
    queue.push_back(ss.texel[i]);
  }
  if (queue.empty()) { std::fill(ss.pad.begin(), ss.pad.end(), -1); return; }
  size_t head = 0;
  static const int dx[8] = {1, -1, 0, 0, 1, 1, -1, -1}, dy[8] = {0, 0, 1, -1, 1, -1, 1, -1};
  while (head < queue.size()) {
    int32_t t = queue[head++];
    int x = t % res, y = t / res;
    int32_t src = ss.pad[t];
    for (int k = 0; k < 8; k++) {
      int nx = x + dx[k], ny = y + dy[k];
      if (nx < 0 || ny < 0 || nx >= res || ny >= res) continue;
      int32_t nt = ny * res + nx;
      if (ss.pad[nt] >= 0) continue;
      ss.pad[nt] = src;
      queue.push_back(nt);
    }
  }
}

void to_image(const SampleSet& ss, const float* samples, int comps, float* out) {
  size_t n = (size_t)ss.res * ss.res;
  parallel_for((int64_t)n, [&](int64_t b, int64_t e) {
    for (int64_t t = b; t < e; t++) {
      int32_t s = ss.pad.empty() ? -1 : ss.pad[t];
      for (int c = 0; c < comps; c++) out[t * comps + c] = s >= 0 ? samples[(size_t)s * comps + c] : 0.f;
    }
  });
}

void from_image(const SampleSet& ss, const float* img, int comps, float* out) {
  parallel_for((int64_t)ss.size(), [&](int64_t b, int64_t e) {
    for (int64_t i = b; i < e; i++)
      for (int c = 0; c < comps; c++) out[i * comps + c] = img[(size_t)ss.texel[i] * comps + c];
  });
}

// Small 3x3 tent blur in texel space (via padding) to remove Monte Carlo grain.
static void denoise(SampleSet& ss, std::vector<float>& v) {
  if (ss.size() == 0) return;
  int res = ss.res;
  std::vector<float> img((size_t)res * res);
  to_image(ss, v.data(), 1, img.data());
  std::vector<float> out(ss.size());
  parallel_for((int64_t)ss.size(), [&](int64_t b, int64_t e) {
    for (int64_t i = b; i < e; i++) {
      int t = ss.texel[i], x = t % res, y = t / res;
      float s = 0, w = 0;
      for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
          int nx = std::clamp(x + dx, 0, res - 1), ny = std::clamp(y + dy, 0, res - 1);
          float ww = (dx == 0 ? 2.f : 1.f) * (dy == 0 ? 2.f : 1.f);
          // only blend with samples on the same triangle island neighborhood (avoid bleeding across UV gaps)
          int32_t src = ss.pad[ny * res + nx];
          if (src < 0 || length2(ss.pos[src] - ss.pos[i]) > 16.f * (ss.len_u[i] * ss.len_u[i] + ss.len_v[i] * ss.len_v[i])) continue;
          s += img[ny * res + nx] * ww;
          w += ww;
        }
      out[i] = w > 0 ? s / w : v[i];
    }
  });
  v.swap(out);
}

// ---------------------------------------------------------------- AO & thickness
static inline float radical_inverse(uint32_t b) {
  b = (b << 16u) | (b >> 16u);
  b = ((b & 0x55555555u) << 1u) | ((b & 0xAAAAAAAAu) >> 1u);
  b = ((b & 0x33333333u) << 2u) | ((b & 0xCCCCCCCCu) >> 2u);
  b = ((b & 0x0F0F0F0Fu) << 4u) | ((b & 0xF0F0F0F0u) >> 4u);
  b = ((b & 0x00FF00FFu) << 8u) | ((b & 0xFF00FF00u) >> 8u);
  return (float)b * 2.3283064365386963e-10f;
}

static void bake_ao(const Mesh& m, const BVH& bvh, SampleSet& ss, const BakeSettings& s) {
  float ext = m.max_extent();
  float maxd = s.ao_distance * ext;
  float eps = ext * 2e-5f;
  int N = s.ao_samples;
  ss.ao.assign(ss.size(), 1.f);
  parallel_for((int64_t)ss.size(), 64, [&](int64_t b, int64_t e) {
    for (int64_t i = b; i < e; i++) {
      vec3 n = ss.nrm[i], fn = ss.fnrm[i];
      vec3 o = ss.pos[i] + fn * eps + n * eps;
      vec3 t, bt;
      onb(n, t, bt);
      float r1 = hash_float((uint32_t)i * 2u + 17u), r2 = hash_float((uint32_t)i * 2u + 91u);
      int hits = 0, valid = 0;
      for (int k = 0; k < N; k++) {
        float u1 = (k + r1) / N;
        float u2 = radical_inverse((uint32_t)k) + r2;
        u2 -= std::floor(u2);
        float r = std::sqrt(u1), phi = 2.f * kPi * u2;
        vec3 d = t * (r * std::cos(phi)) + bt * (r * std::sin(phi)) + n * std::sqrt(std::fmax(0.f, 1.f - u1));
        if (dot(d, fn) < 0.01f) continue;
        valid++;
        if (bvh.occluded(o, d, 0.f, maxd)) hits++;
      }
      ss.ao[i] = valid ? 1.f - (float)hits / valid : 1.f;
    }
  });
  denoise(ss, ss.ao);
}

static void bake_thickness(const Mesh& m, const BVH& bvh, SampleSet& ss, const BakeSettings& s) {
  float ext = m.max_extent();
  float maxd = s.thickness_distance * ext;
  float eps = ext * 2e-5f;
  int N = s.thickness_samples;
  ss.thickness.assign(ss.size(), 1.f);
  parallel_for((int64_t)ss.size(), 64, [&](int64_t b, int64_t e) {
    for (int64_t i = b; i < e; i++) {
      vec3 n = -ss.nrm[i], fn = -ss.fnrm[i];
      vec3 o = ss.pos[i] + fn * eps;
      vec3 t, bt;
      onb(n, t, bt);
      float r1 = hash_float((uint32_t)i * 2u + 7u), r2 = hash_float((uint32_t)i * 2u + 3u);
      float sum = 0;
      int valid = 0;
      for (int k = 0; k < N; k++) {
        float u1 = (k + r1) / N;
        float u2 = radical_inverse((uint32_t)k) + r2;
        u2 -= std::floor(u2);
        // narrower cone than AO (thickness measures "straight through")
        float ct = 1.f - u1 * 0.6f, st = std::sqrt(std::fmax(0.f, 1.f - ct * ct)), phi = 2.f * kPi * u2;
        vec3 d = t * (st * std::cos(phi)) + bt * (st * std::sin(phi)) + n * ct;
        float th, uu, vv;
        uint32_t tri;
        valid++;
        sum += bvh.intersect(o, d, 0.f, maxd, th, tri, uu, vv) ? th : maxd;
      }
      ss.thickness[i] = valid ? saturate(sum / (valid * maxd)) : 1.f;
    }
  });
  denoise(ss, ss.thickness);
}

// ---------------------------------------------------------------- curvature
// Edge-integral curvature: every mesh edge with dihedral angle theta contributes theta * length,
// smoothed by a kernel of radius r in 3D. This gives crisp, resolution-independent convex/concave
// masks on low-poly hard-surface meshes (where vertex curvature fails) and ~mean curvature on
// smooth dense meshes. Output: 0.5 = flat, >0.5 convex, <0.5 concave.
struct EdgeSamples {
  std::vector<vec3> p, n;
  std::vector<float> w;
};

static EdgeSamples curvature_edges(const Mesh& m, float r, float min_angle_deg) {
  uint32_t wc = 0;
  auto weld = weld_by_position(m, &wc);
  struct EdgeRec { uint32_t t0, t1; int count; };
  std::unordered_map<uint64_t, EdgeRec> edges;
  edges.reserve(m.tri_count() * 2);
  for (size_t t = 0; t < m.tri_count(); t++) {
    for (int e = 0; e < 3; e++) {
      uint32_t a = weld[m.idx[t * 3 + e]], b = weld[m.idx[t * 3 + (e + 1) % 3]];
      if (a == b) continue;
      if (a > b) std::swap(a, b);
      uint64_t key = ((uint64_t)a << 32) | b;
      auto it = edges.find(key);
      if (it == edges.end()) edges.emplace(key, EdgeRec{(uint32_t)t, 0, 1});
      else { if (it->second.count == 1) it->second.t1 = (uint32_t)t; it->second.count++; }
    }
  }
  // representative position for each welded vertex
  std::vector<vec3> wpos(wc);
  for (size_t v = 0; v < m.pos.size(); v++) wpos[weld[v]] = m.pos[v];
  auto face_n = [&](uint32_t t) {
    return normalize(cross(m.pos[m.idx[t * 3 + 1]] - m.pos[m.idx[t * 3]], m.pos[m.idx[t * 3 + 2]] - m.pos[m.idx[t * 3]]));
  };
  auto centroid = [&](uint32_t t) { return (m.pos[m.idx[t * 3]] + m.pos[m.idx[t * 3 + 1]] + m.pos[m.idx[t * 3 + 2]]) / 3.f; };
  float min_angle = min_angle_deg * kPi / 180.f;
  float spacing = r * 0.25f;
  EdgeSamples es;
  for (auto& kv : edges) {
    const EdgeRec& er = kv.second;
    if (er.count != 2) continue;
    vec3 n0 = face_n(er.t0), n1 = face_n(er.t1);
    float theta = std::acos(clampf(dot(n0, n1), -1.f, 1.f));
    if (theta < min_angle) continue;
    vec3 a = wpos[(uint32_t)(kv.first >> 32)], b = wpos[(uint32_t)(kv.first & 0xffffffffu)];
    vec3 mid = (a + b) * 0.5f;
    bool convex = dot(n0, centroid(er.t1) - mid) < 0.f;
    float signed_theta = convex ? theta : -theta;
    float L = length(b - a);
    int cnt = std::max(1, (int)std::ceil(L / spacing));
    float seg = L / cnt;
    vec3 en = normalize(n0 + n1);
    for (int k = 0; k < cnt; k++) {
      es.p.push_back(a + (b - a) * ((k + 0.5f) / cnt));
      es.n.push_back(en);
      es.w.push_back(signed_theta * seg);
    }
  }
  return es;
}

static void bake_curvature(const Mesh& m, const EdgeSamples& es, std::vector<SampleSet*>& sets, const BakeSettings& s) {
  float ext = m.max_extent();
  float r = s.curvature_radius * ext;
  vec3 lo = m.bmin - vec3(r), hi = m.bmax + vec3(r);
  vec3 sz = hi - lo;
  float cell = std::fmax(r, std::fmax(sz.x, std::fmax(sz.y, sz.z)) / 192.f);
  int gx = std::max(1, (int)std::ceil(sz.x / cell)), gy = std::max(1, (int)std::ceil(sz.y / cell)), gz = std::max(1, (int)std::ceil(sz.z / cell));
  auto cell_of = [&](vec3 p, int& x, int& y, int& z) {
    x = std::clamp((int)((p.x - lo.x) / cell), 0, gx - 1);
    y = std::clamp((int)((p.y - lo.y) / cell), 0, gy - 1);
    z = std::clamp((int)((p.z - lo.z) / cell), 0, gz - 1);
  };
  size_t ncell = (size_t)gx * gy * gz;
  std::vector<uint32_t> start(ncell + 1, 0);
  std::vector<uint32_t> cell_id(es.p.size());
  for (size_t i = 0; i < es.p.size(); i++) {
    int x, y, z;
    cell_of(es.p[i], x, y, z);
    cell_id[i] = (uint32_t)(((size_t)z * gy + y) * gx + x);
    start[cell_id[i] + 1]++;
  }
  for (size_t c = 0; c < ncell; c++) start[c + 1] += start[c];
  std::vector<uint32_t> order(es.p.size());
  {
    std::vector<uint32_t> fill(start.begin(), start.end() - 1);
    for (size_t i = 0; i < es.p.size(); i++) order[fill[cell_id[i]]++] = (uint32_t)i;
  }
  float r2 = r * r;
  float norm = 3.f / (kPi * r2);  // 1 / integral of (1-d^2/r^2)^2 over the disk
  for (SampleSet* ss : sets) {
    ss->curvature.assign(ss->size(), 0.5f);
    parallel_for((int64_t)ss->size(), 256, [&](int64_t b, int64_t e) {
      for (int64_t i = b; i < e; i++) {
        vec3 p = ss->pos[i], n = ss->nrm[i];
        int cx, cy, cz;
        cell_of(p, cx, cy, cz);
        float acc = 0;
        for (int z = std::max(0, cz - 1); z <= std::min(gz - 1, cz + 1); z++)
          for (int y = std::max(0, cy - 1); y <= std::min(gy - 1, cy + 1); y++)
            for (int x = std::max(0, cx - 1); x <= std::min(gx - 1, cx + 1); x++) {
              size_t c = ((size_t)z * gy + y) * gx + x;
              for (uint32_t k = start[c]; k < start[c + 1]; k++) {
                uint32_t si = order[k];
                float d2 = length2(p - es.p[si]);
                if (d2 >= r2) continue;
                if (dot(es.n[si], n) < -0.05f) continue;  // other side of a thin wall
                float w = 1.f - d2 / r2;
                acc += w * w * es.w[si];
              }
            }
        float c = acc * norm * r * s.curvature_gain;
        ss->curvature[i] = 0.5f + 0.5f * std::tanh(c);
      }
    });
  }
}

// ---------------------------------------------------------------- disk cache
static std::string cache_path(const std::string& dir, uint64_t key) { return path_join(dir, strf("bake-%016llx.bin", (unsigned long long)key)); }

static bool load_cache(const std::string& path, std::vector<SampleSet>& sets) {
  std::string data;
  if (!read_file(path, data)) return false;
  const char* p = data.data();
  const char* end = p + data.size();
  auto rd = [&](void* dst, size_t n) {
    if ((size_t)(end - p) < n) return false;
    memcpy(dst, p, n);
    p += n;
    return true;
  };
  uint32_t magic = 0, count = 0;
  if (!rd(&magic, 4) || magic != 0x31544150u || !rd(&count, 4) || count != sets.size()) return false;
  for (auto& ss : sets) {
    uint64_t n = 0;
    if (!rd(&n, 8) || n != ss.size()) return false;
    ss.ao.resize(n); ss.thickness.resize(n); ss.curvature.resize(n);
    if (!rd(ss.ao.data(), n * 4) || !rd(ss.thickness.data(), n * 4) || !rd(ss.curvature.data(), n * 4)) return false;
  }
  return true;
}

static void save_cache(const std::string& path, const std::vector<SampleSet>& sets) {
  std::string out;
  uint32_t magic = 0x31544150u, count = (uint32_t)sets.size();
  out.append((const char*)&magic, 4);
  out.append((const char*)&count, 4);
  for (auto& ss : sets) {
    uint64_t n = ss.size();
    out.append((const char*)&n, 8);
    out.append((const char*)ss.ao.data(), n * 4);
    out.append((const char*)ss.thickness.data(), n * 4);
    out.append((const char*)ss.curvature.data(), n * 4);
  }
  make_dirs(path_dir(path));
  write_file_or_throw(path, out);
}

// ---------------------------------------------------------------- entry
std::shared_ptr<Baked> bake_mesh(std::shared_ptr<const Mesh> mesh, const std::vector<int>& res_per_set, const BakeSettings& s,
                                 const std::string& cache_dir, bool force) {
  const Mesh& m = *mesh;
  auto bk = std::make_shared<Baked>();
  bk->mesh = mesh;
  bk->settings = s;
  Json stats = Json::object();
  Timer total;

  uint64_t key = m.content_hash ^ s.hash();
  for (int r : res_per_set) key = fnv1a(&r, sizeof r, key);
  bk->key = key;

  Timer t;
  bk->bvh.build(m.pos, m.idx);
  stats.set("bvh_ms", t.ms());

  t = Timer();
  bk->sets.resize(m.set_names.size());
  for (size_t si = 0; si < m.set_names.size(); si++) {
    SampleSet& ss = bk->sets[si];
    ss.name = m.set_names[si];
    ss.set = (int)si;
    ss.res = res_per_set[si];
    raster_set(m, (int)si, ss.res, ss);
    build_padding(ss);
  }
  stats.set("raster_ms", t.ms());

  bool cached = false;
  std::string cpath = cache_dir.empty() ? "" : cache_path(cache_dir, key);
  if (!force && !cpath.empty()) cached = load_cache(cpath, bk->sets);
  if (!cached) {
    t = Timer();
    for (auto& ss : bk->sets) bake_ao(m, bk->bvh, ss, s);
    stats.set("ao_ms", t.ms());
    t = Timer();
    for (auto& ss : bk->sets) bake_thickness(m, bk->bvh, ss, s);
    stats.set("thickness_ms", t.ms());
    t = Timer();
    EdgeSamples es = curvature_edges(m, s.curvature_radius * m.max_extent(), s.curvature_min_angle);
    std::vector<SampleSet*> ptrs;
    for (auto& ss : bk->sets) ptrs.push_back(&ss);
    bake_curvature(m, es, ptrs, s);
    stats.set("curvature_ms", t.ms());
    stats.set("curvature_edge_samples", (int64_t)es.p.size());
    if (!cpath.empty()) {
      try { save_cache(cpath, bk->sets); } catch (const std::exception&) { /* cache is best-effort */ }
    }
  }
  stats.set("from_cache", cached);
  Json sets = Json::object();
  for (auto& ss : bk->sets) {
    Json sj = Json::object();
    sj.set("resolution", ss.res);
    sj.set("samples", (int64_t)ss.size());
    sj.set("coverage", (double)ss.interior_count / ((double)ss.res * ss.res));
    sets.set(ss.name, sj);
  }
  stats.set("texture_sets", sets);
  stats.set("total_ms", total.ms());
  stats.set("threads", thread_count());
  bk->stats = stats;
  return bk;
}

}  // namespace pt
