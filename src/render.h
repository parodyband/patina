// Headless CPU PBR preview renderer (deterministic; works without a GPU) + contact sheets.
#pragma once
#include "bvh.h"
#include "eval.h"
#include "mesh.h"

namespace pt {

struct ViewSpec {
  std::string name;
  float azimuth = 0, elevation = 0;  // degrees; azimuth 0 = front (+Z), 90 = right (+X)
  float zoom = 1;
  bool has_target = false;
  vec3 target_bbox{0.5f, 0.5f, 0.5f};  // look-at point in bbox coordinates
  bool has_world_target = false;
  vec3 target_world{0, 0, 0};
};
std::vector<ViewSpec> parse_views(const Json& views);  // "front,iso", ["front", {"azimuth":30,"elevation":20}], ...
std::vector<std::string> view_names();

struct RenderOptions {
  int size = 512;  // per view (square)
  int width = 0, height = 0;  // overrides size when set (viewer)
  int ssaa = 2;
  std::string mode = "lit";  // see render_modes()
  bool shadows = true;
  bool labels = true;
  int columns = 0;
  std::string title;
  float exposure = 1.f;
  std::string environment = "studio";  // built-in HDRI | "procedural" (analytic studio) | path to an .hdr
  float env_rotation = 0.f;            // degrees around +Y
  float env_intensity = 1.f;
};
Json render_modes();

struct RgbImage {
  int w = 0, h = 0;
  std::vector<uint8_t> px;  // RGB8
};

// maps must be indexed by texture set (maps[i].set == i) or be empty for set i.
RgbImage render_view(const Mesh& m, const BVH& bvh, const std::vector<SetMaps>& maps, const ViewSpec& v, const RenderOptions& o);
RgbImage render_views(const Mesh& m, const BVH& bvh, const std::vector<SetMaps>& maps, const std::vector<ViewSpec>& views, const RenderOptions& o);
RgbImage render_texture_sheet(const std::vector<SetMaps>& maps, int thumb, const std::string& title);
RgbImage compose_grid(const std::vector<RgbImage>& tiles, const std::vector<std::string>& labels, int columns, const std::string& title);
void draw_text(RgbImage& img, int x, int y, const std::string& text, uint8_t r, uint8_t g, uint8_t b, int scale);

}  // namespace pt
