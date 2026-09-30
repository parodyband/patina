// Layer-stack evaluation: fields (generators/noise/paint/decals), mask stacks, blending, channels.
#pragma once
#include <unordered_map>
#include <unordered_set>

#include "bake.h"
#include "json.h"

namespace pt {

struct Project;

enum Chan { C_BASECOLOR, C_METALLIC, C_ROUGHNESS, C_NORMAL, C_HEIGHT, C_AO, C_EMISSIVE, C_OPACITY, C_COUNT };
struct ChannelInfo {
  const char* name;
  int comps;
  float def[3];  // default (linear; basecolor default is sRGB 0.5 gray)
  bool color;    // authored as sRGB colors
  const char* doc;
};
extern const ChannelInfo kChannels[C_COUNT];
int channel_index(const std::string& name);

struct Stack {
  size_t n = 0;
  std::vector<float> ch[C_COUNT];
  bool used[C_COUNT] = {};
  void init(size_t n);
};

struct EvalOptions {
  std::unordered_set<std::string> record_masks;  // layer ids whose effective masks to keep; "*" = all
};

struct SetResult {
  int set = 0;
  std::string name;
  int res = 0;
  Stack stack;  // final per-sample channels; normal includes height detail; ao includes baked AO
  std::unordered_map<std::string, std::vector<float>> masks;
  std::vector<std::string> warnings;
  Json stats;
};

SetResult evaluate_set(const Project& proj, const Baked& bk, int set, const EvalOptions& opt);

// Full-resolution padded images for rendering and export.
struct SetMaps {
  std::string name;
  int set = 0;
  int res = 0;
  std::vector<float> ch[C_COUNT];  // comps * res * res
  bool used[C_COUNT] = {};
  std::unordered_map<std::string, std::vector<float>> extra;  // 1-comp debug maps (masks, bake maps)
};
SetMaps make_maps(const SampleSet& ss, const SetResult& r, const std::vector<std::string>& extra_maps);

// Documentation of every field type / generator (used by `library` and validation).
Json field_catalog();
Json blend_mode_list();
std::vector<std::string> field_type_names();

}  // namespace pt
