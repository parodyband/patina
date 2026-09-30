#include "image.h"

#include <cstdlib>
#include <mutex>
#include <unordered_map>

#include "stb_image.h"
#include "stb_image_write.h"

extern "C" unsigned char* stbi_zlib_compress(unsigned char* data, int data_len, int* out_len, int quality);

namespace pt {

vec4 Image::texel(int x, int y) const {
  const float* p = &px[((size_t)y * w + x) * c];
  switch (c) {
    case 1: return {p[0], p[0], p[0], 1};
    case 2: return {p[0], p[0], p[0], p[1]};
    case 3: return {p[0], p[1], p[2], 1};
    default: return {p[0], p[1], p[2], p[3]};
  }
}

vec4 Image::sample_bilinear(float u, float v, bool wrap) const {
  if (w == 0 || h == 0) return {0, 0, 0, 0};
  float x = u * w - 0.5f, y = v * h - 0.5f;
  float fx = std::floor(x), fy = std::floor(y);
  int x0 = (int)fx, y0 = (int)fy;
  float tx = x - fx, ty = y - fy;
  auto fix = [&](int i, int n) {
    if (wrap) { i %= n; if (i < 0) i += n; return i; }
    return i < 0 ? 0 : (i >= n ? n - 1 : i);
  };
  int xa = fix(x0, w), xb = fix(x0 + 1, w), ya = fix(y0, h), yb = fix(y0 + 1, h);
  vec4 a = texel(xa, ya), b = texel(xb, ya), c2 = texel(xa, yb), d = texel(xb, yb);
  auto l = [](vec4 p, vec4 q, float t) { return vec4(p.x + (q.x - p.x) * t, p.y + (q.y - p.y) * t, p.z + (q.z - p.z) * t, p.w + (q.w - p.w) * t); };
  return l(l(a, b, tx), l(c2, d, tx), ty);
}

Image load_image(const std::string& path) {
  Image img;
  int w, h, c;
  if (stbi_is_16_bit(path.c_str())) {
    stbi_us* data = stbi_load_16(path.c_str(), &w, &h, &c, 0);
    if (!data) fail("cannot load image '%s': %s", path.c_str(), stbi_failure_reason());
    img.w = w; img.h = h; img.c = c;
    img.px.resize((size_t)w * h * c);
    for (size_t i = 0; i < img.px.size(); i++) img.px[i] = data[i] / 65535.f;
    stbi_image_free(data);
  } else {
    stbi_uc* data = stbi_load(path.c_str(), &w, &h, &c, 0);
    if (!data) fail("cannot load image '%s': %s", path.c_str(), stbi_failure_reason());
    img.w = w; img.h = h; img.c = c;
    img.px.resize((size_t)w * h * c);
    for (size_t i = 0; i < img.px.size(); i++) img.px[i] = data[i] / 255.f;
    stbi_image_free(data);
  }
  return img;
}

std::shared_ptr<const Image> cached_image(const std::string& path) {
  static std::mutex m;
  struct Entry { int64_t mtime; std::shared_ptr<const Image> img; };
  static std::unordered_map<std::string, Entry> cache;
  std::string key = path_abs(path);
  int64_t mt = file_mtime_ns(key);
  {
    std::lock_guard<std::mutex> lk(m);
    auto it = cache.find(key);
    if (it != cache.end() && it->second.mtime == mt) return it->second.img;
  }
  if (!file_exists(key)) fail("image not found: '%s'", path.c_str());
  auto img = std::make_shared<const Image>(load_image(key));
  std::lock_guard<std::mutex> lk(m);
  if (cache.size() > 64) cache.clear();
  cache[key] = {mt, img};
  return img;
}

// ---------------------------------------------------------------- PNG writing
static uint32_t crc_table[256];
static void crc_init() {
  static bool done = false;
  if (done) return;
  for (uint32_t n = 0; n < 256; n++) {
    uint32_t c = n;
    for (int k = 0; k < 8; k++) c = (c & 1) ? 0xedb88320U ^ (c >> 1) : c >> 1;
    crc_table[n] = c;
  }
  done = true;
}
static uint32_t crc32(const uint8_t* p, size_t n, uint32_t c = 0xffffffffU) {
  for (size_t i = 0; i < n; i++) c = crc_table[(c ^ p[i]) & 0xff] ^ (c >> 8);
  return c;
}
static void put32(std::string& s, uint32_t v) {
  s += (char)(v >> 24); s += (char)(v >> 16); s += (char)(v >> 8); s += (char)v;
}
static void chunk(std::string& out, const char* type, const uint8_t* data, size_t n) {
  put32(out, (uint32_t)n);
  size_t start = out.size();
  out.append(type, 4);
  if (n) out.append((const char*)data, n);
  uint32_t c = crc32((const uint8_t*)out.data() + start, n + 4) ^ 0xffffffffU;
  put32(out, c);
}

// Generic PNG encoder (8 or 16 bit) using stb's zlib and the Paeth/Sub filter heuristic.
static std::string encode_png(int w, int h, int c, int bits, const uint8_t* raw /* big-endian samples */) {
  static std::once_flag once;
  std::call_once(once, crc_init);
  int bpp = c * (bits / 8);
  size_t stride = (size_t)w * bpp;
  std::vector<uint8_t> filt((stride + 1) * h);
  // per-row filter selection (min sum of abs), rows in parallel
  parallel_for(h, 16, [&](int64_t y0, int64_t y1) {
    std::vector<uint8_t> cand(stride);
    for (int64_t y = y0; y < y1; y++) {
      const uint8_t* row = raw + y * stride;
      const uint8_t* prev = y > 0 ? raw + (y - 1) * stride : nullptr;
      uint8_t* dst = &filt[y * (stride + 1)];
      long best = -1;
      for (int f = 0; f < 5; f++) {
        long sum = 0;
        for (size_t i = 0; i < stride; i++) {
          int a = i >= (size_t)bpp ? row[i - bpp] : 0;
          int b = prev ? prev[i] : 0;
          int cc = (prev && i >= (size_t)bpp) ? prev[i - bpp] : 0;
          int v;
          switch (f) {
            case 0: v = row[i]; break;
            case 1: v = row[i] - a; break;
            case 2: v = row[i] - b; break;
            case 3: v = row[i] - ((a + b) >> 1); break;
            default: {
              int p = a + b - cc, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - cc);
              int pr = (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : cc);
              v = row[i] - pr;
            }
          }
          cand[i] = (uint8_t)v;
          sum += std::abs((int)(int8_t)(uint8_t)v);
        }
        if (best < 0 || sum < best) {
          best = sum;
          dst[0] = (uint8_t)f;
          memcpy(dst + 1, cand.data(), stride);
        }
      }
    }
  });
  int zlen = 0;
  unsigned char* z = stbi_zlib_compress(filt.data(), (int)filt.size(), &zlen, 5);
  if (!z) fail("PNG compression failed");
  std::string out("\x89PNG\r\n\x1a\n", 8);
  uint8_t ihdr[13];
  ihdr[0] = w >> 24; ihdr[1] = w >> 16; ihdr[2] = w >> 8; ihdr[3] = w;
  ihdr[4] = h >> 24; ihdr[5] = h >> 16; ihdr[6] = h >> 8; ihdr[7] = h;
  ihdr[8] = (uint8_t)bits;
  static const uint8_t ctype[5] = {0, 0, 4, 2, 6};
  ihdr[9] = ctype[c];
  ihdr[10] = ihdr[11] = ihdr[12] = 0;
  chunk(out, "IHDR", ihdr, 13);
  chunk(out, "IDAT", z, (size_t)zlen);
  chunk(out, "IEND", nullptr, 0);
  free(z);
  return out;
}

void save_png(const std::string& path, int w, int h, int c, const float* data, bool srgb, int bits) {
  if (c < 1 || c > 4) fail("save_png: bad channel count %d", c);
  size_t n = (size_t)w * h * c;
  std::vector<uint8_t> raw(n * (bits / 8));
  parallel_for((int64_t)((size_t)w * h), [&](int64_t b, int64_t e) {
    for (int64_t i = b; i < e; i++)
      for (int k = 0; k < c; k++) {
        float v = data[i * c + k];
        if (srgb && k < 3 && (c >= 3 || c == 1)) v = linear_to_srgb(v);
        v = saturate(v);
        size_t o = (size_t)i * c + k;
        if (bits == 16) {
          uint16_t q = (uint16_t)std::lround(v * 65535.f);
          raw[o * 2] = (uint8_t)(q >> 8);
          raw[o * 2 + 1] = (uint8_t)q;
        } else {
          raw[o] = (uint8_t)std::lround(v * 255.f);
        }
      }
  });
  std::string png = encode_png(w, h, c, bits, raw.data());
  write_file_or_throw(path, png);
}

std::string encode_png_rgb8(int w, int h, const uint8_t* rgb) { return encode_png(w, h, 3, 8, rgb); }
std::string encode_png8(int w, int h, int comps, const uint8_t* px) { return encode_png(w, h, comps, 8, px); }

void save_png_rgb8(const std::string& path, int w, int h, const uint8_t* rgb) {
  write_file_or_throw(path, encode_png_rgb8(w, h, rgb));
}

}  // namespace pt
