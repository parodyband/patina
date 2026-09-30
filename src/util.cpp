#include "core.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <algorithm>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace pt {

mat4 operator*(const mat4& a, const mat4& b) {
  mat4 r;
  for (int c = 0; c < 4; c++)
    for (int rr = 0; rr < 4; rr++) {
      float s = 0;
      for (int k = 0; k < 4; k++) s += a.m[k * 4 + rr] * b.m[c * 4 + k];
      r.m[c * 4 + rr] = s;
    }
  return r;
}

mat4 look_at(vec3 eye, vec3 target, vec3 up) {
  vec3 f = normalize(target - eye);
  vec3 s = normalize(cross(f, up));
  vec3 u = cross(s, f);
  mat4 r;
  r.m[0] = s.x; r.m[4] = s.y; r.m[8] = s.z;
  r.m[1] = u.x; r.m[5] = u.y; r.m[9] = u.z;
  r.m[2] = -f.x; r.m[6] = -f.y; r.m[10] = -f.z;
  r.m[12] = -dot(s, eye); r.m[13] = -dot(u, eye); r.m[14] = dot(f, eye);
  return r;
}

mat4 perspective(float fovy, float aspect, float zn, float zf) {
  mat4 r;
  float f = 1.0f / std::tan(fovy * 0.5f);
  std::memset(r.m, 0, sizeof(r.m));
  r.m[0] = f / aspect;
  r.m[5] = f;
  r.m[10] = (zf + zn) / (zn - zf);
  r.m[11] = -1.f;
  r.m[14] = 2.f * zf * zn / (zn - zf);
  return r;
}

uint64_t fnv1a(const void* data, size_t n, uint64_t h) {
  const uint8_t* p = (const uint8_t*)data;
  for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
  return h;
}
uint32_t hash_string(const std::string& s) { uint64_t h = fnv1a(s); return (uint32_t)(h ^ (h >> 32)); }

void fail(const char* fmt, ...) {
  char buf[4096];
  va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
  throw Error(buf);
}

std::string strf(const char* fmt, ...) {
  va_list ap; va_start(ap, fmt);
  va_list ap2; va_copy(ap2, ap);
  int n = vsnprintf(nullptr, 0, fmt, ap);
  va_end(ap);
  std::string s(n > 0 ? n : 0, '\0');
  vsnprintf(s.data(), s.size() + 1, fmt, ap2);
  va_end(ap2);
  return s;
}

std::string to_lower(std::string s) {
  for (auto& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

bool glob_match(const char* p, const char* t) {
  // iterative wildcard match with backtracking on the last '*'
  const char *star = nullptr, *ss = nullptr;
  while (*t) {
    if (*p == '?' || std::tolower((unsigned char)*p) == std::tolower((unsigned char)*t)) { p++; t++; continue; }
    if (*p == '*') { star = p++; ss = t; continue; }
    if (star) { p = star + 1; t = ++ss; continue; }
    return false;
  }
  while (*p == '*') p++;
  return !*p;
}

int edit_distance(const std::string& a, const std::string& b) {
  std::vector<int> prev(b.size() + 1), cur(b.size() + 1);
  for (size_t j = 0; j <= b.size(); j++) prev[j] = (int)j;
  for (size_t i = 1; i <= a.size(); i++) {
    cur[0] = (int)i;
    for (size_t j = 1; j <= b.size(); j++) {
      int c = std::tolower((unsigned char)a[i - 1]) == std::tolower((unsigned char)b[j - 1]) ? 0 : 1;
      cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + c});
    }
    std::swap(prev, cur);
  }
  return prev[b.size()];
}

std::string did_you_mean(const std::string& word, const std::vector<std::string>& cands) {
  std::string best;
  int bestd = 1 << 30;
  for (auto& c : cands) {
    int d = edit_distance(word, c);
    if (d < bestd) { bestd = d; best = c; }
  }
  int limit = std::max<int>(2, (int)word.size() / 3);
  return bestd <= limit ? best : std::string();
}

std::string sanitize_filename(const std::string& s) {
  std::string r;
  for (char c : s) r += (std::isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.') ? c : '_';
  if (r.empty()) r = "unnamed";
  return r;
}

std::string base64_encode(const void* data, size_t n) {
  static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  const uint8_t* p = (const uint8_t*)data;
  std::string out;
  out.reserve((n + 2) / 3 * 4);
  size_t i = 0;
  for (; i + 2 < n; i += 3) {
    uint32_t v = (p[i] << 16) | (p[i + 1] << 8) | p[i + 2];
    out += tbl[v >> 18]; out += tbl[(v >> 12) & 63]; out += tbl[(v >> 6) & 63]; out += tbl[v & 63];
  }
  if (i < n) {
    uint32_t v = p[i] << 16;
    if (i + 1 < n) v |= p[i + 1] << 8;
    out += tbl[v >> 18]; out += tbl[(v >> 12) & 63];
    out += (i + 1 < n) ? tbl[(v >> 6) & 63] : '=';
    out += '=';
  }
  return out;
}

double now_seconds() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// ---------------------------------------------------------------- filesystem
bool read_file(const std::string& path, std::string& out) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return false;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  out.resize(n > 0 ? (size_t)n : 0);
  size_t got = n > 0 ? fread(out.data(), 1, (size_t)n, f) : 0;
  fclose(f);
  return got == out.size();
}

void write_file_or_throw(const std::string& path, const void* data, size_t n) {
  // write to a temp file then rename, so concurrent readers never see partial files
  std::string tmp = path + strf(".tmp%u", (unsigned)hash_u32((uint32_t)(now_seconds() * 1e6)));
  FILE* f = fopen(tmp.c_str(), "wb");
  if (!f) fail("cannot write '%s'", path.c_str());
  size_t w = fwrite(data, 1, n, f);
  fclose(f);
  if (w != n) { std::remove(tmp.c_str()); fail("short write to '%s'", path.c_str()); }
  std::error_code ec;
  fs::rename(tmp, path, ec);
  if (ec) {
    fs::remove(path, ec);
    fs::rename(tmp, path, ec);
    if (ec) fail("cannot move temp file to '%s': %s", path.c_str(), ec.message().c_str());
  }
}

bool file_exists(const std::string& p) { std::error_code ec; return fs::exists(p, ec); }
bool is_directory(const std::string& p) { std::error_code ec; return fs::is_directory(p, ec); }
int64_t file_mtime_ns(const std::string& p) {
  std::error_code ec;
  auto t = fs::last_write_time(p, ec);
  if (ec) return 0;
  return (int64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
}
int64_t file_size(const std::string& p) { std::error_code ec; auto s = fs::file_size(p, ec); return ec ? -1 : (int64_t)s; }
void make_dirs(const std::string& p) {
  if (p.empty()) return;
  std::error_code ec;
  fs::create_directories(p, ec);
  if (ec && !is_directory(p)) fail("cannot create directory '%s': %s", p.c_str(), ec.message().c_str());
}
std::string path_dir(const std::string& p) { return fs::path(p).parent_path().string(); }
std::string path_filename(const std::string& p) { return fs::path(p).filename().string(); }
std::string path_stem(const std::string& p) {
  std::string s = fs::path(p).filename().string();
  // strip compound extensions like .patina.json
  size_t dot = s.find('.');
  return dot == std::string::npos || dot == 0 ? s : s.substr(0, dot);
}
std::string path_ext(const std::string& p) { return to_lower(fs::path(p).extension().string()); }
std::string path_join(const std::string& a, const std::string& b) {
  if (a.empty()) return b;
  fs::path pb(b);
  if (pb.is_absolute()) return b;
  return (fs::path(a) / pb).lexically_normal().string();
}
std::string path_abs(const std::string& p) {
  std::error_code ec;
  auto r = fs::absolute(p, ec);
  return ec ? p : r.lexically_normal().string();
}
std::string path_relative(const std::string& p, const std::string& base) {
  std::error_code ec;
  auto r = fs::relative(path_abs(p), path_abs(base.empty() ? "." : base), ec);
  return ec || r.empty() ? p : r.generic_string();
}
std::vector<std::string> list_dir(const std::string& p, const std::string& ext) {
  std::vector<std::string> out;
  std::error_code ec;
  for (auto& e : fs::directory_iterator(p, ec)) {
    if (!e.is_regular_file()) continue;
    if (!ext.empty() && path_ext(e.path().string()) != ext) continue;
    out.push_back(e.path().string());
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::string exe_path() {
#if defined(__APPLE__)
  char buf[4096];
  uint32_t sz = sizeof buf;
  if (_NSGetExecutablePath(buf, &sz) == 0) return path_abs(buf);
  return "";
#elif defined(_WIN32)
  char buf[4096];
  DWORD n = GetModuleFileNameA(nullptr, buf, sizeof buf);
  return std::string(buf, n);
#else
  char buf[4096];
  ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
  return n > 0 ? std::string(buf, n) : "";
#endif
}

}  // namespace pt
