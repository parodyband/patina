#include "json.h"
#include "core.h"

#include <cstdio>
#include <algorithm>
#include <cstdlib>

namespace pt {

static const Json kNull;
static const std::string kEmpty;

const char* Json::type_name() const {
  switch (t_) {
    case Null: return "null";
    case Bool: return "boolean";
    case Number: return "number";
    case String: return "string";
    case Array: return "array";
    case Object: return "object";
  }
  return "?";
}

const std::string& Json::as_str() const { return t_ == String ? s_ : kEmpty; }

const Json& Json::operator[](size_t i) const { return (t_ == Array && i < a_.size()) ? a_[i] : kNull; }

void Json::push(Json v) {
  if (t_ != Array) { *this = array(); }
  a_.push_back(std::move(v));
}
void Json::insert(size_t index, Json v) {
  if (t_ != Array) *this = array();
  if (index > a_.size()) index = a_.size();
  a_.insert(a_.begin() + index, std::move(v));
}
void Json::erase_at(size_t index) {
  if (t_ == Array && index < a_.size()) a_.erase(a_.begin() + index);
}

const Json* Json::find(const std::string& k) const {
  if (t_ != Object) return nullptr;
  for (auto& kv : o_) if (kv.first == k) return &kv.second;
  return nullptr;
}
Json* Json::find(const std::string& k) {
  if (t_ != Object) return nullptr;
  for (auto& kv : o_) if (kv.first == k) return &kv.second;
  return nullptr;
}
const Json& Json::operator[](const std::string& k) const {
  const Json* v = find(k);
  return v ? *v : kNull;
}
Json& Json::set(const std::string& k, Json v) {
  if (t_ != Object) *this = object();
  for (auto& kv : o_) if (kv.first == k) { kv.second = std::move(v); return kv.second; }
  o_.emplace_back(k, std::move(v));
  return o_.back().second;
}
Json& Json::ref(const std::string& k) {
  if (t_ != Object) *this = object();
  for (auto& kv : o_) if (kv.first == k) return kv.second;
  o_.emplace_back(k, Json());
  return o_.back().second;
}
bool Json::erase(const std::string& k) {
  if (t_ != Object) return false;
  for (size_t i = 0; i < o_.size(); i++)
    if (o_[i].first == k) { o_.erase(o_.begin() + i); return true; }
  return false;
}
std::vector<std::string> Json::keys() const {
  std::vector<std::string> r;
  for (auto& kv : o_) r.push_back(kv.first);
  return r;
}

bool Json::operator==(const Json& o) const {
  if (t_ != o.t_) return false;
  switch (t_) {
    case Null: return true;
    case Bool: return b_ == o.b_;
    case Number: return n_ == o.n_;
    case String: return s_ == o.s_;
    case Array: return a_ == o.a_;
    case Object: {
      if (o_.size() != o.o_.size()) return false;
      for (auto& kv : o_) {
        const Json* v = o.find(kv.first);
        if (!v || !(*v == kv.second)) return false;
      }
      return true;
    }
  }
  return false;
}

void Json::merge_patch(const Json& patch) {
  if (!patch.is_object()) { *this = patch; return; }
  if (!is_object()) *this = object();
  for (auto& kv : patch.o_) {
    if (kv.second.is_null()) erase(kv.first);
    else ref(kv.first).merge_patch(kv.second);
  }
}

// ---------------------------------------------------------------- writer
static void dump_string(std::string& out, const std::string& s) {
  out += '"';
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      default:
        if (c < 0x20) { char buf[8]; snprintf(buf, sizeof buf, "\\u%04x", c); out += buf; }
        else out += (char)c;
    }
  }
  out += '"';
}

static void dump_number(std::string& out, double n) {
  if (!std::isfinite(n)) { out += "null"; return; }
  if (n == (double)(int64_t)n && std::fabs(n) < 1e15) {
    char buf[32]; snprintf(buf, sizeof buf, "%lld", (long long)n); out += buf; return;
  }
  char buf[40];
  // shortest representation that round-trips at float-ish precision keeps files readable
  snprintf(buf, sizeof buf, "%.7g", n);
  if (strtod(buf, nullptr) != n) snprintf(buf, sizeof buf, "%.17g", n);
  out += buf;
}

// Arrays of scalars are printed on one line to keep documents compact and diff-friendly.
static bool is_flat_array(const std::vector<Json>& a) {
  if (a.size() > 16) return false;
  for (auto& v : a) if (v.is_array() || v.is_object()) return false;
  return true;
}

void Json::dump_to(std::string& out, int indent, int depth) const {
  auto nl = [&](int d) {
    if (indent < 0) return;
    out += '\n';
    out.append((size_t)(indent * d), ' ');
  };
  switch (t_) {
    case Null: out += "null"; break;
    case Bool: out += b_ ? "true" : "false"; break;
    case Number: dump_number(out, n_); break;
    case String: dump_string(out, s_); break;
    case Array: {
      if (a_.empty()) { out += "[]"; break; }
      bool flat = indent < 0 || is_flat_array(a_);
      out += '[';
      for (size_t i = 0; i < a_.size(); i++) {
        if (i) out += flat && indent >= 0 ? ", " : ",";
        if (!flat) nl(depth + 1);
        a_[i].dump_to(out, indent, depth + 1);
      }
      if (!flat) nl(depth);
      out += ']';
      break;
    }
    case Object: {
      if (o_.empty()) { out += "{}"; break; }
      out += '{';
      for (size_t i = 0; i < o_.size(); i++) {
        if (i) out += ',';
        nl(depth + 1);
        dump_string(out, o_[i].first);
        out += indent >= 0 ? ": " : ":";
        o_[i].second.dump_to(out, indent, depth + 1);
      }
      nl(depth);
      out += '}';
      break;
    }
  }
}

std::string Json::dump(int indent) const {
  std::string out;
  dump_to(out, indent, 0);
  return out;
}

// ---------------------------------------------------------------- parser
namespace {
struct Parser {
  const char* p;
  const char* begin;
  const char* end;
  std::string err;

  void error(const char* msg) {
    if (!err.empty()) return;
    int line = 1, col = 1;
    for (const char* q = begin; q < p && q < end; q++) {
      if (*q == '\n') { line++; col = 1; } else col++;
    }
    err = strf("JSON parse error at line %d, column %d: %s", line, col, msg);
  }
  void ws() {
    for (;;) {
      while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
      // tolerate // line comments (handy in hand-edited project files)
      if (p + 1 < end && p[0] == '/' && p[1] == '/') { while (p < end && *p != '\n') p++; continue; }
      break;
    }
  }
  bool lit(const char* s) {
    size_t n = strlen(s);
    if ((size_t)(end - p) >= n && memcmp(p, s, n) == 0) { p += n; return true; }
    return false;
  }
  static void utf8(std::string& o, uint32_t cp) {
    if (cp < 0x80) o += (char)cp;
    else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 63)); }
    else if (cp < 0x10000) { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 63)); o += (char)(0x80 | (cp & 63)); }
    else { o += (char)(0xF0 | (cp >> 18)); o += (char)(0x80 | ((cp >> 12) & 63)); o += (char)(0x80 | ((cp >> 6) & 63)); o += (char)(0x80 | (cp & 63)); }
  }
  bool hex4(uint32_t& v) {
    if (end - p < 4) return false;
    v = 0;
    for (int i = 0; i < 4; i++) {
      char c = *p++;
      v <<= 4;
      if (c >= '0' && c <= '9') v |= c - '0';
      else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
      else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
      else return false;
    }
    return true;
  }
  bool string(std::string& o) {
    if (p >= end || *p != '"') { error("expected string"); return false; }
    p++;
    while (p < end && *p != '"') {
      char c = *p++;
      if (c == '\\') {
        if (p >= end) break;
        char e = *p++;
        switch (e) {
          case '"': o += '"'; break;
          case '\\': o += '\\'; break;
          case '/': o += '/'; break;
          case 'b': o += '\b'; break;
          case 'f': o += '\f'; break;
          case 'n': o += '\n'; break;
          case 'r': o += '\r'; break;
          case 't': o += '\t'; break;
          case 'u': {
            uint32_t cp;
            if (!hex4(cp)) { error("bad \\u escape"); return false; }
            if (cp >= 0xD800 && cp < 0xDC00 && p + 1 < end && p[0] == '\\' && p[1] == 'u') {
              p += 2;
              uint32_t lo;
              if (!hex4(lo)) { error("bad surrogate"); return false; }
              cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            }
            utf8(o, cp);
            break;
          }
          default: error("bad escape"); return false;
        }
      } else {
        o += c;
      }
    }
    if (p >= end) { error("unterminated string"); return false; }
    p++;
    return true;
  }
  bool value(Json& v, int depth) {
    if (depth > 256) { error("nesting too deep"); return false; }
    ws();
    if (p >= end) { error("unexpected end of input"); return false; }
    char c = *p;
    if (c == '{') {
      p++;
      v = Json::object();
      ws();
      if (p < end && *p == '}') { p++; return true; }
      for (;;) {
        ws();
        std::string k;
        if (!string(k)) return false;
        ws();
        if (p >= end || *p != ':') { error("expected ':' after object key"); return false; }
        p++;
        Json item;
        if (!value(item, depth + 1)) return false;
        v.set(k, std::move(item));
        ws();
        if (p < end && *p == ',') {
          p++;
          ws();
          if (p < end && *p == '}') { p++; return true; }  // tolerate trailing comma
          continue;
        }
        if (p < end && *p == '}') { p++; return true; }
        error("expected ',' or '}' in object");
        return false;
      }
    }
    if (c == '[') {
      p++;
      v = Json::array();
      ws();
      if (p < end && *p == ']') { p++; return true; }
      for (;;) {
        Json item;
        if (!value(item, depth + 1)) return false;
        v.push(std::move(item));
        ws();
        if (p < end && *p == ',') {
          p++;
          ws();
          if (p < end && *p == ']') { p++; return true; }
          continue;
        }
        if (p < end && *p == ']') { p++; return true; }
        error("expected ',' or ']' in array");
        return false;
      }
    }
    if (c == '"') {
      std::string s;
      if (!string(s)) return false;
      v = Json(std::move(s));
      return true;
    }
    if (lit("true")) { v = Json(true); return true; }
    if (lit("false")) { v = Json(false); return true; }
    if (lit("null")) { v = Json(); return true; }
    if (c == '-' || (c >= '0' && c <= '9')) {
      char* e = nullptr;
      std::string tmp(p, std::min<size_t>(64, end - p));
      double d = strtod(tmp.c_str(), &e);
      if (e == tmp.c_str()) { error("bad number"); return false; }
      p += (e - tmp.c_str());
      v = Json(d);
      return true;
    }
    error("unexpected character");
    return false;
  }
};
}  // namespace

bool Json::try_parse(const std::string& text, Json& out, std::string& err) {
  Parser ps{text.data(), text.data(), text.data() + text.size(), {}};
  if (!ps.value(out, 0)) { err = ps.err; return false; }
  ps.ws();
  if (ps.p != ps.end) { ps.error("trailing characters after JSON value"); err = ps.err; return false; }
  return true;
}

Json Json::parse(const std::string& text) {
  Json j;
  std::string err;
  if (!try_parse(text, j, err)) throw Error(err);
  return j;
}

}  // namespace pt
