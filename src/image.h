// Float images, PNG I/O (8/16-bit), and a thread-safe cache for images referenced by layers.
#pragma once
#include "core.h"
#include <memory>

namespace pt {

struct Image {
  int w = 0, h = 0, c = 0;  // c = channels (1..4)
  std::vector<float> px;    // row-major, values in [0,1] as stored in the file (no color conversion)
  bool has_alpha() const { return c == 2 || c == 4; }
  vec4 texel(int x, int y) const;  // missing channels: gray replicates, alpha=1
  vec4 sample_bilinear(float u, float v, bool wrap) const;  // u,v in [0,1], v down
};

Image load_image(const std::string& path);  // throws
std::shared_ptr<const Image> cached_image(const std::string& path);  // reloads when the file changes

// Writes interleaved float data (c = 1..4) to PNG. If `srgb`, channels 0..2 are converted linear->sRGB.
void save_png(const std::string& path, int w, int h, int c, const float* data, bool srgb, int bits = 8);
std::string encode_png_rgb8(int w, int h, const uint8_t* rgb);  // in-memory PNG (for MCP image content)
std::string encode_png8(int w, int h, int comps, const uint8_t* px);  // in-memory 8-bit PNG, 1..4 channels
void save_png_rgb8(const std::string& path, int w, int h, const uint8_t* rgb);

}  // namespace pt
