// Patina core: math, hashing, errors, parallelism, filesystem helpers.
#pragma once
#include <cstdint>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
#include <functional>
#include <stdexcept>

namespace pt {

// ---------------------------------------------------------------- math
struct vec2 {
  float x = 0, y = 0;
  vec2() = default;
  constexpr vec2(float x_, float y_) : x(x_), y(y_) {}
  vec2 operator+(vec2 o) const { return {x + o.x, y + o.y}; }
  vec2 operator-(vec2 o) const { return {x - o.x, y - o.y}; }
  vec2 operator*(float s) const { return {x * s, y * s}; }
};

struct vec3 {
  float x = 0, y = 0, z = 0;
  vec3() = default;
  constexpr vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
  explicit constexpr vec3(float s) : x(s), y(s), z(s) {}
  vec3 operator+(vec3 o) const { return {x + o.x, y + o.y, z + o.z}; }
  vec3 operator-(vec3 o) const { return {x - o.x, y - o.y, z - o.z}; }
  vec3 operator*(vec3 o) const { return {x * o.x, y * o.y, z * o.z}; }
  vec3 operator/(vec3 o) const { return {x / o.x, y / o.y, z / o.z}; }
  vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
  vec3 operator/(float s) const { float r = 1.0f / s; return {x * r, y * r, z * r}; }
  vec3 operator-() const { return {-x, -y, -z}; }
  vec3& operator+=(vec3 o) { x += o.x; y += o.y; z += o.z; return *this; }
  vec3& operator-=(vec3 o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
  vec3& operator*=(float s) { x *= s; y *= s; z *= s; return *this; }
  float operator[](int i) const { return (&x)[i]; }
  float& operator[](int i) { return (&x)[i]; }
};

struct vec4 {
  float x = 0, y = 0, z = 0, w = 0;
  vec4() = default;
  constexpr vec4(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}
  vec4(vec3 v, float w_) : x(v.x), y(v.y), z(v.z), w(w_) {}
  vec3 xyz() const { return {x, y, z}; }
};

inline vec3 operator*(float s, vec3 v) { return v * s; }
inline float dot(vec3 a, vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline vec3 cross(vec3 a, vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float length(vec3 v) { return std::sqrt(dot(v, v)); }
inline float length2(vec3 v) { return dot(v, v); }
inline vec3 normalize(vec3 v) { float l = length(v); return l > 1e-20f ? v / l : vec3(0, 0, 1); }
inline vec3 vmin(vec3 a, vec3 b) { return {std::fmin(a.x, b.x), std::fmin(a.y, b.y), std::fmin(a.z, b.z)}; }
inline vec3 vmax(vec3 a, vec3 b) { return {std::fmax(a.x, b.x), std::fmax(a.y, b.y), std::fmax(a.z, b.z)}; }
inline vec3 lerp(vec3 a, vec3 b, float t) { return a + (b - a) * t; }
inline float lerp(float a, float b, float t) { return a + (b - a) * t; }
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float saturate(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }
inline float smoothstep(float e0, float e1, float x) {
  if (e0 == e1) return x < e0 ? 0.f : 1.f;
  float t = saturate((x - e0) / (e1 - e0));
  return t * t * (3.f - 2.f * t);
}
inline float srgb_to_linear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
inline float linear_to_srgb(float c) {
  c = saturate(c);
  return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.f / 2.4f) - 0.055f;
}
constexpr float kPi = 3.14159265358979323846f;

// Orthonormal basis from a unit normal (Duff et al. 2017).
inline void onb(vec3 n, vec3& t, vec3& b) {
  float s = n.z >= 0 ? 1.f : -1.f;
  float a = -1.f / (s + n.z);
  float bb = n.x * n.y * a;
  t = {1.f + s * n.x * n.x * a, s * bb, -s * n.x};
  b = {bb, s + n.y * n.y * a, -n.y};
}

// 4x4 column-major matrix (only what the renderer needs).
struct mat4 {
  float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  vec4 mul(vec4 v) const {
    return {m[0] * v.x + m[4] * v.y + m[8] * v.z + m[12] * v.w, m[1] * v.x + m[5] * v.y + m[9] * v.z + m[13] * v.w,
            m[2] * v.x + m[6] * v.y + m[10] * v.z + m[14] * v.w, m[3] * v.x + m[7] * v.y + m[11] * v.z + m[15] * v.w};
  }
  vec3 mul_point(vec3 p) const { vec4 r = mul(vec4(p, 1)); return {r.x, r.y, r.z}; }
  vec3 mul_dir(vec3 d) const { vec4 r = mul(vec4(d, 0)); return {r.x, r.y, r.z}; }
};
mat4 operator*(const mat4& a, const mat4& b);
mat4 look_at(vec3 eye, vec3 target, vec3 up);
mat4 perspective(float fovy_rad, float aspect, float znear, float zfar);

// ---------------------------------------------------------------- hashing
inline uint32_t hash_u32(uint32_t x) {
  x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
  return x;
}
inline uint32_t hash_combine(uint32_t a, uint32_t b) { return hash_u32(a ^ (b + 0x9e3779b9U + (a << 6) + (a >> 2))); }
inline float hash_float(uint32_t x) { return (hash_u32(x) >> 8) * (1.0f / 16777216.0f); }
uint64_t fnv1a(const void* data, size_t n, uint64_t h = 14695981039346656037ull);
inline uint64_t fnv1a(const std::string& s, uint64_t h = 14695981039346656037ull) { return fnv1a(s.data(), s.size(), h); }
uint32_t hash_string(const std::string& s);

// ---------------------------------------------------------------- errors & strings
struct Error : std::runtime_error {
  using std::runtime_error::runtime_error;
};
[[noreturn]] void fail(const char* fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;
std::string strf(const char* fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;
std::string to_lower(std::string s);
bool glob_match(const char* pattern, const char* text);  // '*' and '?' wildcards, case-insensitive
int edit_distance(const std::string& a, const std::string& b);
// Returns the closest candidate within a reasonable edit distance, or "".
std::string did_you_mean(const std::string& word, const std::vector<std::string>& candidates);
std::string sanitize_filename(const std::string& s);
std::string base64_encode(const void* data, size_t n);

// ---------------------------------------------------------------- time
double now_seconds();
struct Timer {
  double t0 = now_seconds();
  double ms() const { return (now_seconds() - t0) * 1000.0; }
};

// ---------------------------------------------------------------- parallelism
int thread_count();
// Runs fn(begin, end) over [0, n) split into chunks of `grain`, on all cores. Safe to call
// concurrently from many threads and nested; the caller participates in the work.
void parallel_for(int64_t n, int64_t grain, const std::function<void(int64_t, int64_t)>& fn);
// Convenience: auto grain.
inline void parallel_for(int64_t n, const std::function<void(int64_t, int64_t)>& fn) {
  int64_t grain = n / (thread_count() * 8);
  if (grain < 256) grain = 256;
  parallel_for(n, grain, fn);
}

// ---------------------------------------------------------------- filesystem
bool read_file(const std::string& path, std::string& out);
void write_file_or_throw(const std::string& path, const void* data, size_t n);
inline void write_file_or_throw(const std::string& path, const std::string& s) { write_file_or_throw(path, s.data(), s.size()); }
bool file_exists(const std::string& path);
bool is_directory(const std::string& path);
int64_t file_mtime_ns(const std::string& path);
int64_t file_size(const std::string& path);
void make_dirs(const std::string& path);
std::string path_dir(const std::string& path);
std::string path_filename(const std::string& path);
std::string path_stem(const std::string& path);
std::string path_ext(const std::string& path);  // lowercase, with dot
std::string path_join(const std::string& a, const std::string& b);
std::string path_abs(const std::string& path);
std::string path_relative(const std::string& path, const std::string& base_dir);
std::vector<std::string> list_dir(const std::string& path, const std::string& ext_filter = "");
std::string exe_path();

}  // namespace pt
