// Tangent-space normal baking.
//
// High -> low: every texel of the low mesh casts rays from an automatic cage (the low surface pushed out
// along position-welded, averaged normals, so rays stay continuous across hard edges and there are no
// gaps) back through the surface, and takes the first high-poly hit. Ray direction ("skew"): averaged
// cage normals everywhere make details on large faces project at an angle (Substance's skew). Here the
// direction turns from the cage normal at hard edges to the low's shading normal away from them
// (skew = auto), or blends by a fixed amount. Parts are matched by name (Foo_low <- Foo_high) and each
// group traces its own acceleration structure, so a part can never pick up another part's surface.
//
// Bevel shader: rounded edges from ray sampling (after Cycles' Bevel node): rays through a disk around
// the point along the normal and both tangents, multiple importance sampling between the three
// projections and a cubic falloff. Only runs near feature edges; stays within the same part by default.
//
// Results are denoised with an edge-avoiding a-trous filter in UV space (guided by 3D position and
// normal, so nothing blurs across seams or islands), encoded in the low's MikkTSpace basis, and turned
// into extra curvature so edge wear follows baked edges.
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

#include "bake.h"

namespace pt {

void NormalBakeSettings::from_json(const Json& j) {
  if (!j.is_object()) return;
  high = j.str("high", high);
  match = to_lower(j.str("match", match));
  if (match != "name" && match != "all") fail("bake.normal.match must be \"name\" or \"all\"");
  cage = std::clamp(j.numf("cage", cage), 0.f, 1.f);
  depth = std::clamp(j.numf("depth", depth), 0.f, 1.f);
  if (const Json* sk = j.find("skew")) {
    if (sk->is_string() && to_lower(sk->as_str()) == "auto") skew = -1.f;
    else if (sk->is_number()) skew = saturate(sk->as_float());
    else fail("bake.normal.skew must be \"auto\" or 0..1");
  }
  skew_distance = std::clamp(j.numf("skew_distance", skew_distance), 1e-4f, 1.f);
  ignore_backfaces = j.boolean("ignore_backfaces", ignore_backfaces);
  samples = std::clamp(j.integer("samples", samples), 1, 64);
  denoise = j.boolean("denoise", denoise);
  curvature = std::clamp(j.numf("curvature", curvature), 0.f, 10.f);
  if (const Json* b = j.find("bevel")) {
    if (b->is_number()) bevel_radius = b->as_float();
    else if (b->is_object()) {
      bevel_radius = b->numf("radius", 0.006f);
      bevel_samples = std::clamp(b->integer("samples", bevel_samples), 4, 1024);
      bevel_min_angle = std::clamp(b->numf("min_angle", bevel_min_angle), 0.f, 90.f);
      bevel_same_part = b->str("scope", "part") != "all";
    } else fail("bake.normal.bevel must be a radius or {\"radius\":0.006,\"samples\":32,\"scope\":\"part\"|\"all\"}");
    bevel_radius = std::clamp(bevel_radius, 0.f, 0.25f);
  }
}

Json NormalBakeSettings::to_json() const {
  Json j = Json::object();
  j.set("high", high);
  j.set("match", match);
  j.set("cage", cage);
  j.set("depth", depth);
  j.set("skew", skew < 0 ? Json("auto") : Json(skew));
  j.set("skew_distance", skew_distance);
  j.set("ignore_backfaces", ignore_backfaces);
  j.set("samples", samples);
  j.set("denoise", denoise);
  j.set("curvature", curvature);
  Json b = Json::object();
  b.set("radius", bevel_radius);
  b.set("samples", bevel_samples);
  b.set("min_angle", bevel_min_angle);
  b.set("scope", bevel_same_part ? "part" : "all");
  j.set("bevel", b);
  return j;
}

namespace {

inline vec3 tri_normal(const Mesh& m, uint32_t t) {
  vec3 a = m.pos[m.idx[t * 3]], b = m.pos[m.idx[t * 3 + 1]], c = m.pos[m.idx[t * 3 + 2]];
  return normalize(cross(b - a, c - a));
}

inline vec3 barycentric(const Mesh& m, uint32_t t, vec3 p) {
  vec3 a = m.pos[m.idx[t * 3]], b = m.pos[m.idx[t * 3 + 1]], c = m.pos[m.idx[t * 3 + 2]];
  vec3 v0 = b - a, v1 = c - a, v2 = p - a;
  float d00 = dot(v0, v0), d01 = dot(v0, v1), d11 = dot(v1, v1), d20 = dot(v2, v0), d21 = dot(v2, v1);
  float den = d00 * d11 - d01 * d01;
  if (std::fabs(den) < 1e-30f) return {1.f / 3, 1.f / 3, 1.f / 3};
  float v = (d11 * d20 - d01 * d21) / den, w = (d00 * d21 - d01 * d20) / den;
  return {1.f - v - w, v, w};
}

inline vec3 smooth_normal(const Mesh& m, uint32_t t, float u, float v) {
  vec3 n = m.nrm[m.idx[t * 3]] * (1.f - u - v) + m.nrm[m.idx[t * 3 + 1]] * u + m.nrm[m.idx[t * 3 + 2]] * v;
  return normalize(n);
}

// Bake group of a part name (Toolbag's rule, case-insensitive): everything before the first "high"/"low"
// token (also hi/lo/hp/lp) delimited by '_', '.', '-' or ' '; anything after it is a variation of the same
// group, so Plate_high_bolts joins Plate. Blender ".001" duplicate suffixes are ignored.
std::string match_name(std::string s) {
  s = to_lower(s);
  while (s.size() > 4 && s[s.size() - 4] == '.' && std::isdigit((unsigned char)s[s.size() - 1]) &&
         std::isdigit((unsigned char)s[s.size() - 2]) && std::isdigit((unsigned char)s[s.size() - 3]))
    s.resize(s.size() - 4);
  auto delim = [](char c) { return c == '_' || c == '.' || c == '-' || c == ' '; };
  size_t i = 0;
  while (i < s.size()) {
    size_t j = i;
    while (j < s.size() && !delim(s[j])) j++;
    std::string tok = s.substr(i, j - i);
    if (i > 0 && (tok == "high" || tok == "low" || tok == "hi" || tok == "lo" || tok == "hp" || tok == "lp")) return s.substr(0, i - 1);
    i = j + 1;
  }
  return s;
}

// A BVH over a subset of a mesh's triangles, reporting original triangle ids.
struct SubBVH {
  BVH bvh;
  std::vector<uint32_t> tri;  // local -> original triangle
  void build(const Mesh& m, const std::vector<uint32_t>& tris) {
    tri = tris;
    std::vector<uint32_t> idx;
    idx.reserve(tris.size() * 3);
    for (uint32_t t : tris) for (int k = 0; k < 3; k++) idx.push_back(m.idx[t * 3 + k]);
    if (!idx.empty()) bvh.build(m.pos, idx);
  }
  bool hit(vec3 o, vec3 d, float tmin, float tmax, float& t, uint32_t& orig, float& u, float& v) const {
    if (bvh.empty()) return false;
    uint32_t local;
    if (!bvh.intersect(o, d, tmin, tmax, t, local, u, v)) return false;
    orig = tri[local];
    return true;
  }
};

// Uniform grid of segments for "distance to the nearest edge" queries within `radius`.
struct EdgeGrid {
  float cell = 1, radius = 1;
  std::unordered_map<uint64_t, std::vector<uint32_t>> cells;
  std::vector<vec3> a, b;
  static uint64_t key(int x, int y, int z) {
    return ((uint64_t)(uint32_t)(x + 1048576) << 42) ^ ((uint64_t)(uint32_t)(y + 1048576) << 21) ^ (uint64_t)(uint32_t)(z + 1048576);
  }
  void build(float r) {
    radius = r;
    cell = std::fmax(r, 1e-6f);
    for (uint32_t s = 0; s < a.size(); s++) {
      vec3 d = b[s] - a[s];
      int steps = std::max(1, (int)std::ceil(length(d) / (cell * 0.5f)));
      uint64_t last = UINT64_MAX;
      for (int k = 0; k <= steps; k++) {
        vec3 p = a[s] + d * ((float)k / steps);
        int cx = (int)std::floor(p.x / cell), cy = (int)std::floor(p.y / cell), cz = (int)std::floor(p.z / cell);
        for (int z = cz - 1; z <= cz + 1; z++)
          for (int y = cy - 1; y <= cy + 1; y++)
            for (int x = cx - 1; x <= cx + 1; x++) {
              auto& v = cells[key(x, y, z)];
              if (v.empty() || v.back() != s) v.push_back(s);
            }
        (void)last;
      }
    }
  }
  // distance to the nearest segment, capped at `radius`
  float distance(vec3 p) const {
    auto it = cells.find(key((int)std::floor(p.x / cell), (int)std::floor(p.y / cell), (int)std::floor(p.z / cell)));
    float best = radius;
    if (it == cells.end()) return best;
    for (uint32_t s : it->second) {
      vec3 d = b[s] - a[s];
      float t = saturate(dot(p - a[s], d) / std::fmax(dot(d, d), 1e-20f));
      best = std::fmin(best, length(p - (a[s] + d * t)));
    }
    return best;
  }
};

// Edges of `m` where the surface creases (dihedral > min_angle_deg) or the normals are split (hard edges).
void feature_edges(const Mesh& m, const std::vector<uint32_t>& weld, float min_angle_deg, bool split_only, EdgeGrid& out) {
  struct E { uint32_t t; vec3 na, nb; uint32_t wa; };
  std::unordered_map<uint64_t, E> first;
  float cos_min = std::cos(min_angle_deg * kPi / 180.f);
  for (uint32_t t = 0; t < m.tri_count(); t++) {
    for (int k = 0; k < 3; k++) {
      uint32_t va = m.idx[t * 3 + k], vb = m.idx[t * 3 + (k + 1) % 3];
      uint32_t wa = weld[va], wb = weld[vb];
      if (wa == wb) continue;
      uint64_t key = wa < wb ? ((uint64_t)wa << 32 | wb) : ((uint64_t)wb << 32 | wa);
      auto it = first.find(key);
      if (it == first.end()) { first.emplace(key, E{t, m.nrm[va], m.nrm[vb], wa}); continue; }
      const E& e = it->second;
      vec3 na = e.wa == wa ? e.na : e.nb, nb = e.wa == wa ? e.nb : e.na;
      bool split = dot(na, m.nrm[va]) < 0.999f || dot(nb, m.nrm[vb]) < 0.999f;
      bool crease = !split_only && dot(tri_normal(m, e.t), tri_normal(m, t)) < cos_min;
      if (split || crease) { out.a.push_back(m.pos[va]); out.b.push_back(m.pos[vb]); }
    }
  }
}

inline vec2 hammersley2(uint32_t i, uint32_t n, uint32_t scramble) {
  uint32_t b = i;
  b = (b << 16u) | (b >> 16u);
  b = ((b & 0x55555555u) << 1u) | ((b & 0xAAAAAAAAu) >> 1u);
  b = ((b & 0x33333333u) << 2u) | ((b & 0xCCCCCCCCu) >> 2u);
  b = ((b & 0x0F0F0F0Fu) << 4u) | ((b & 0xF0F0F0F0u) >> 4u);
  b = ((b & 0x00FF00FFu) << 8u) | ((b & 0xFF00FF00u) >> 8u);
  float x = (i + hash_float(scramble)) / n;
  return {x - std::floor(x), (float)(b ^ scramble) * 2.3283064365386963e-10f};
}

// Bevel shader: a rounded-edge normal at p (geometric normal ng) from surfaces of `bvh` within `R`.
vec3 bevel_normal(const Mesh& m, const SubBVH& bvh, vec3 p, vec3 ng, vec3 fallback, float R, int S, uint32_t seed) {
  vec3 t0, b0;
  onb(ng, t0, b0);
  const vec3 axes[3] = {ng, t0, b0};
  const float axis_p[3] = {0.5f, 0.25f, 0.25f};
  vec3 sum{0, 0, 0};
  float eps = R * 1e-4f;
  for (int j = 0; j < S; j++) {
    // stratified projection axis (half along the normal, a quarter along each tangent)
    int ax = j * 4 < S * 2 ? 0 : (j * 4 < S * 3 ? 1 : 2);
    int j0 = ax == 0 ? 0 : (ax == 1 ? S / 2 : S * 3 / 4), jn = ax == 0 ? S / 2 : (ax == 1 ? S * 3 / 4 - S / 2 : S - S * 3 / 4);
    vec2 xi = hammersley2((uint32_t)(j - j0), (uint32_t)std::max(jn, 1), seed + (uint32_t)ax * 0x68bc21ebu);
    vec3 dN = axes[ax], dT = axes[(ax + 1) % 3], dB = axes[(ax + 2) % 3];
    float r = R * std::sqrt(xi.x), phi = 2.f * kPi * xi.y;
    vec3 origin = p + dT * (r * std::cos(phi)) + dB * (r * std::sin(phi)) + dN * R;
    float tmin = 0.f;
    for (int h = 0; h < 4; h++) {  // every surface the probe crosses within the disk's slab
      float t, u, v;
      uint32_t tri;
      if (!bvh.hit(origin, -dN, tmin, 2.f * R, t, tri, u, v)) break;
      tmin = t + eps;
      vec3 hp = origin - dN * t;
      vec3 d = hp - p;
      float dist = length(d);
      if (dist >= R) continue;
      vec3 hg = tri_normal(m, tri);
      vec3 hn = smooth_normal(m, tri, u, v);
      if (dot(hn, ng) < -0.25f) continue;  // back side of a thin wall
      // power-heuristic MIS over the three projections (as Cycles): w = p_sampled / sum(p_k^2)
      float p2 = 0, ps = axis_p[ax] * std::fabs(dot(axes[ax], hg));
      for (int k = 0; k < 3; k++) {
        vec3 dp = d - axes[k] * dot(d, axes[k]);
        if (dot(dp, dp) < R * R) { float pk = axis_p[k] * std::fabs(dot(axes[k], hg)); p2 += pk * pk; }
      }
      if (p2 <= 1e-8f) continue;
      float f = 1.f - dist / R;
      sum += hn * (f * f * f * ps / p2);
    }
  }
  return length2(sum) > 1e-12f ? normalize(sum) : fallback;
}

// Edge-avoiding a-trous (B3 spline, steps 1,2,4,8) over per-sample vectors in UV space.
void denoise_vectors(const SampleSet& ss, std::vector<vec3>& v, const std::vector<uint8_t>& active) {
  int res = ss.res;
  static const float k[5] = {1.f / 16, 1.f / 4, 3.f / 8, 1.f / 4, 1.f / 16};
  std::vector<vec3> cur = v, next(v.size());
  for (int step : {1, 2, 4, 8}) {
    parallel_for((int64_t)ss.size(), 256, [&](int64_t b, int64_t e) {
      for (int64_t i = b; i < e; i++) {
        if (!active[i]) { next[i] = cur[i]; continue; }
        int t = ss.texel[i], x = t % res, y = t / res;
        float texel = std::fmax(ss.len_u[i], ss.len_v[i]);
        float sp2 = 2.f * (texel * step) * (texel * step) * 2.f;
        vec3 acc{0, 0, 0};
        float wsum = 0;
        for (int dy = -2; dy <= 2; dy++)
          for (int dx = -2; dx <= 2; dx++) {
            int nx = x + dx * step, ny = y + dy * step;
            if (nx < 0 || ny < 0 || nx >= res || ny >= res) continue;
            int32_t s = ss.pad[(size_t)ny * res + nx];
            if (s < 0) continue;
            float w = k[dx + 2] * k[dy + 2];
            w *= std::exp(-length2(ss.pos[s] - ss.pos[i]) / sp2);           // same place in 3D
            w *= std::pow(std::fmax(0.f, dot(ss.fnrm[s], ss.fnrm[i])), 32.f);  // same face orientation
            acc += cur[s] * w;
            wsum += w;
          }
        next[i] = wsum > 0 ? normalize(acc) : cur[i];
      }
    });
    cur.swap(next);
  }
  v.swap(cur);
}

}  // namespace

void bake_normals(const Mesh& low, const Mesh* high, std::vector<SampleSet>& sets, const BakeSettings& bs, Json& stats) {
  const NormalBakeSettings& s = bs.normal;
  Timer total;
  float ext = low.max_extent();
  float cage = s.cage * ext, depth = s.depth * ext, R = s.bevel_radius * ext;
  float eps = ext * 1e-6f;
  Json st = Json::object();

  // averaged (cage) normals: angle-weighted face normals per welded position
  uint32_t nweld = 0;
  std::vector<uint32_t> weld = weld_by_position(low, &nweld);
  std::vector<vec3> avg(nweld, vec3(0.f));
  for (uint32_t t = 0; t < low.tri_count(); t++) {
    vec3 fn = tri_normal(low, t);
    for (int k = 0; k < 3; k++) {
      vec3 p = low.pos[low.idx[t * 3 + k]], a = low.pos[low.idx[t * 3 + (k + 1) % 3]], b = low.pos[low.idx[t * 3 + (k + 2) % 3]];
      float ang = std::acos(clampf(dot(normalize(a - p), normalize(b - p)), -1.f, 1.f));
      avg[weld[low.idx[t * 3 + k]]] += fn * ang;
    }
  }
  for (auto& n : avg) n = normalize(n);

  // hard edges (split normals) for auto skew
  EdgeGrid hard;
  bool auto_skew = s.skew < 0 && high;
  if (auto_skew) {
    feature_edges(low, weld, 180.f, true, hard);
    hard.build(s.skew_distance * ext);
  }
  // feature edges for the bevel shader (low only; with a high mesh the bevel runs on the high)
  EdgeGrid feat_low;
  if (R > 0 && !high) {
    feature_edges(low, weld, s.bevel_min_angle, false, feat_low);
    feat_low.build(R);
  }

  // high-poly groups: low part -> BVH of matching high parts
  std::vector<SubBVH> groups;
  std::vector<int> low_group(low.part_names.size(), -1);
  Json gj = Json::array();
  std::vector<std::string> warnings;
  std::vector<uint32_t> weld_high;
  EdgeGrid feat_high;
  if (high) {
    std::vector<std::vector<uint32_t>> high_tris_of_part(high->part_names.size());
    for (uint32_t t = 0; t < high->tri_count(); t++) high_tris_of_part[high->tri_part[t]].push_back(t);
    std::vector<uint8_t> high_used(high->part_names.size(), 0);
    std::vector<uint32_t> all;
    for (uint32_t t = 0; t < high->tri_count(); t++) all.push_back(t);
    int all_group = -1;
    auto get_all = [&]() {
      if (all_group < 0) { groups.emplace_back(); groups.back().build(*high, all); all_group = (int)groups.size() - 1; }
      return all_group;
    };
    for (size_t lp = 0; lp < low.part_names.size(); lp++) {
      Json g = Json::object();
      g.set("low", low.part_names[lp]);
      Json hs = Json::array();
      if (s.match == "name") {
        std::string base = match_name(low.part_names[lp]);
        std::vector<uint32_t> tris;
        for (size_t hp = 0; hp < high->part_names.size(); hp++)
          if (match_name(high->part_names[hp]) == base) {
            tris.insert(tris.end(), high_tris_of_part[hp].begin(), high_tris_of_part[hp].end());
            high_used[hp] = 1;
            hs.push(high->part_names[hp]);
          }
        if (!tris.empty()) {
          groups.emplace_back();
          groups.back().build(*high, tris);
          low_group[lp] = (int)groups.size() - 1;
        } else {
          low_group[lp] = get_all();
          hs.push("*");
          warnings.push_back("low part '" + low.part_names[lp] + "' has no matching high part (name it '" + base +
                             "_high'); it bakes against the whole high mesh");
        }
      } else {
        low_group[lp] = get_all();
        hs.push("*");
      }
      g.set("high", hs);
      gj.push(g);
    }
    if (s.match == "name")
      for (size_t hp = 0; hp < high->part_names.size(); hp++)
        if (!high_used[hp] && all_group < 0) warnings.push_back("high part '" + high->part_names[hp] + "' matches no low part and is ignored");
    if (R > 0) {
      weld_high = weld_by_position(*high, nullptr);
      feature_edges(*high, weld_high, s.bevel_min_angle, false, feat_high);
      feat_high.build(R);
    }
  }
  // per-part BVHs for the bevel shader
  const Mesh& bev_mesh = high ? *high : low;
  std::vector<SubBVH> part_bvh;
  SubBVH whole_bvh;
  if (R > 0) {
    if (s.bevel_same_part) {
      std::vector<std::vector<uint32_t>> tris(bev_mesh.part_names.size());
      for (uint32_t t = 0; t < bev_mesh.tri_count(); t++) tris[bev_mesh.tri_part[t]].push_back(t);
      part_bvh.resize(tris.size());
      for (size_t k = 0; k < tris.size(); k++) part_bvh[k].build(bev_mesh, tris[k]);
    } else {
      std::vector<uint32_t> all;
      for (uint32_t t = 0; t < bev_mesh.tri_count(); t++) all.push_back(t);
      whole_bvh.build(bev_mesh, all);
    }
  }
  auto bevel_at = [&](vec3 p, uint32_t tri, vec3 shading, uint32_t seed) {
    const EdgeGrid& fg = high ? feat_high : feat_low;
    if (fg.distance(p) >= R) return shading;  // nothing to round within reach
    const SubBVH& b = s.bevel_same_part ? part_bvh[bev_mesh.tri_part[tri]] : whole_bvh;
    return bevel_normal(bev_mesh, b, p, tri_normal(bev_mesh, tri), shading, R, s.bevel_samples, seed);
  };

  int grid = std::max(1, (int)std::lround(std::sqrt((float)s.samples)));
  int64_t misses_total = 0, samples_total = 0;
  Json sets_j = Json::object();
  for (auto& ss : sets) {
    size_t n = ss.size();
    ss.nmap.assign(n, vec3(0, 0, 1));
    ss.nmiss.assign(n, 0);
    std::vector<uint8_t> denoise_mask(n, 0);
    std::vector<vec3> world(n);
    std::atomic<int64_t> misses{0};
    parallel_for((int64_t)n, 64, [&](int64_t b, int64_t e) {
      for (int64_t i = b; i < e; i++) {
        uint32_t t = ss.tri[i];
        vec3 bc = barycentric(low, t, ss.pos[i]);
        vec3 ns = normalize(ss.nrm[i]);
        vec3 nc = normalize(avg[weld[low.idx[t * 3]]] * bc.x + avg[weld[low.idx[t * 3 + 1]]] * bc.y + avg[weld[low.idx[t * 3 + 2]]] * bc.z);
        vec3 tg = ss.tan[i].xyz(), bt = cross(ns, tg) * ss.tan[i].w;
        uint32_t seed = hash_u32((uint32_t)ss.texel[i] * 2654435761u + (uint32_t)ss.set * 97u);
        vec3 acc{0, 0, 0};
        int miss = 0;
        bool bevelled = false;
        if (high) {
          float w = s.skew >= 0 ? s.skew : smoothstep(0.f, s.skew_distance * ext, hard.distance(ss.pos[i]));
          vec3 dir = normalize(lerp(nc, ns, w));
          const SubBVH& g = groups[low_group[low.tri_part[t]]];
          for (int sy = 0; sy < grid; sy++)
            for (int sx = 0; sx < grid; sx++) {
              float ox = (sx + 0.5f) / grid - 0.5f, oy = (sy + 0.5f) / grid - 0.5f;
              vec3 p = ss.pos[i] + tg * (ox * ss.len_u[i]) - bt * (oy * ss.len_v[i]);
              // auto: at hard edges dir == nc on both sides, so starting along dir is gap-free and never shifts
              // details sideways; a fixed skew starts on the continuous cage (Toolbag) so edges stay closed
              vec3 o = s.skew < 0 ? p + dir * cage : p + nc * cage;
              float tmax = s.skew < 0 ? cage + depth : (cage + depth) / std::fmax(dot(nc, dir), 0.25f);
              float tmin = 0.f, th, u, v;
              uint32_t ht;
              vec3 nh;
              bool found = false;
              for (int k = 0; k < 8; k++) {
                if (!g.hit(o, -dir, tmin, tmax, th, ht, u, v)) break;
                if (s.ignore_backfaces && dot(tri_normal(*high, ht), dir) < 0.f) { tmin = th + eps; continue; }
                nh = smooth_normal(*high, ht, u, v);
                if (R > 0) {
                  vec3 nb = bevel_at(o - dir * th, ht, nh, seed + (uint32_t)(sy * grid + sx) * 7919u);
                  bevelled = bevelled || dot(nb, nh) < 0.99999f;
                  nh = nb;
                }
                found = true;
                break;
              }
              if (!found) { miss++; nh = ns; }
              acc += nh;
            }
        } else {
          vec3 nb = bevel_at(ss.pos[i], t, ns, seed);
          bevelled = feat_low.distance(ss.pos[i]) < R;  // denoise the whole band so it fades out smoothly
          acc = nb;
        }
        if (miss * 2 > grid * grid) { ss.nmiss[i] = 1; misses++; }
        world[i] = normalize(acc);
        denoise_mask[i] = bevelled;
      }
    });
    if (s.denoise && R > 0) denoise_vectors(ss, world, denoise_mask);
    // encode in the low's tangent space: the exact inverse of a MikkTSpace shader, which uses the
    // unnormalized interpolated vT, vN and vB = sign * cross(vN, vT): n = normalize(x vT + y vB + z vN)
    parallel_for((int64_t)n, 256, [&](int64_t b, int64_t e) {
      for (int64_t i = b; i < e; i++) {
        uint32_t t = ss.tri[i];
        vec3 bc = barycentric(low, t, ss.pos[i]);
        uint32_t i0 = low.idx[t * 3], i1 = low.idx[t * 3 + 1], i2 = low.idx[t * 3 + 2];
        vec3 vN = low.nrm[i0] * bc.x + low.nrm[i1] * bc.y + low.nrm[i2] * bc.z;
        vec3 vT = low.tan[i0].xyz() * bc.x + low.tan[i1].xyz() * bc.y + low.tan[i2].xyz() * bc.z;
        vec3 vB = cross(vN, vT) * low.tan[i0].w;
        vec3 r0 = cross(vB, vN), r1 = cross(vN, vT), r2 = cross(vT, vB);
        float sg = dot(vT, r0) < 0 ? -1.f : 1.f;
        vec3 w = world[i];
        vec3 nt = vec3(dot(w, r0), dot(w, r1), dot(w, r2)) * sg;
        if (length2(nt) < 1e-20f) nt = vec3(0, 0, 1);
        nt = normalize(nt);
        nt.z = std::fmax(nt.z, 1e-3f);
        ss.nmap[i] = normalize(nt);
      }
    });
    Json sj = Json::object();
    sj.set("misses", (int64_t)misses.load());
    sj.set("miss_fraction", n ? (double)misses.load() / n : 0.0);
    sets_j.set(ss.name, sj);
    misses_total += misses.load();
    samples_total += (int64_t)n;
  }

  // curvature from the baked normals: divergence of the tangent-space normal over the curvature radius,
  // added to the geometric curvature so edge wear and dirt follow baked edges and details
  if (s.curvature > 0) {
    float rc = bs.curvature_radius * ext;
    for (auto& ss : sets) {
      int res = ss.res;
      std::vector<float> img((size_t)res * res * 3);
      std::vector<float> flat(ss.size() * 3);
      for (size_t i = 0; i < ss.size(); i++) { flat[i * 3] = ss.nmap[i].x; flat[i * 3 + 1] = ss.nmap[i].y; flat[i * 3 + 2] = ss.nmap[i].z; }
      to_image(ss, flat.data(), 3, img.data());
      std::vector<float> div(ss.size(), 0.f);
      parallel_for((int64_t)ss.size(), 256, [&](int64_t b, int64_t e) {
        for (int64_t i = b; i < e; i++) {
          int t = ss.texel[i], x = t % res, y = t / res;
          float lim = 3.f * std::fmax(ss.len_u[i], ss.len_v[i]);
          auto at = [&](int xx, int yy, int c) {
            xx = std::clamp(xx, 0, res - 1);
            yy = std::clamp(yy, 0, res - 1);
            int32_t s2 = ss.pad[(size_t)yy * res + xx];
            if (s2 < 0 || length2(ss.pos[s2] - ss.pos[i]) > lim * lim) return img[(size_t)t * 3 + c];  // across a seam: one-sided
            return img[((size_t)yy * res + xx) * 3 + c];
          };
          float dx = (at(x + 1, y, 0) - at(x - 1, y, 0)) / (2.f * ss.len_u[i]);
          float dy = (at(x, y - 1, 1) - at(x, y + 1, 1)) / (2.f * ss.len_v[i]);
          div[i] = dx + dy;
        }
      });
      // average over the curvature radius (box, separable in texture space), then add to the geometric term
      int rad = std::max(1, (int)std::lround(rc / std::fmax(1e-9f, ss.len_u.empty() ? 1.f : ss.len_u[ss.size() / 2])));
      rad = std::min(rad, 32);
      std::vector<float> dimg((size_t)res * res), tmp((size_t)res * res);
      to_image(ss, div.data(), 1, dimg.data());
      for (int pass = 0; pass < 2; pass++) {
        parallel_for(res, 4, [&](int64_t y0, int64_t y1) {
          for (int64_t y = y0; y < y1; y++)
            for (int x = 0; x < res; x++) {
              float a = 0;
              for (int k = -rad; k <= rad; k++) {
                int xx = pass == 0 ? std::clamp(x + k, 0, res - 1) : x, yy = pass == 0 ? (int)y : std::clamp((int)y + k, 0, res - 1);
                a += dimg[(size_t)yy * res + xx];
              }
              tmp[(size_t)y * res + x] = a / (2 * rad + 1);
            }
        });
        dimg.swap(tmp);
      }
      std::vector<float> avgdiv(ss.size());
      from_image(ss, dimg.data(), 1, avgdiv.data());
      for (size_t i = 0; i < ss.size(); i++) {
        float c = clampf(ss.curvature[i] * 2.f - 1.f, -0.999f, 0.999f);
        float pre = 0.5f * std::log((1.f + c) / (1.f - c));  // atanh
        pre += avgdiv[i] * rc * s.curvature * bs.curvature_gain;
        ss.curvature[i] = 0.5f + 0.5f * std::tanh(pre);
      }
    }
  }

  st.set("sets", sets_j);
  st.set("miss_fraction", samples_total ? (double)misses_total / samples_total : 0.0);
  if (high) st.set("groups", gj);
  Json wj = Json::array();
  for (auto& w : warnings) wj.push(w);
  st.set("warnings", wj);
  st.set("ms", total.ms());
  stats.set("normal", st);
}

}  // namespace pt
