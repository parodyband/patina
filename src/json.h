// Minimal ordered JSON DOM with a strict parser (with line/col errors) and pretty printer.
#pragma once
#include <string>
#include <vector>
#include <utility>
#include <cstdint>

namespace pt {

class Json {
 public:
  enum Type : uint8_t { Null, Bool, Number, String, Array, Object };

  Json() = default;
  Json(std::nullptr_t) {}
  Json(bool v) : t_(Bool), b_(v) {}
  Json(int v) : t_(Number), n_(v) {}
  Json(unsigned v) : t_(Number), n_(v) {}
  Json(int64_t v) : t_(Number), n_((double)v) {}
  Json(uint64_t v) : t_(Number), n_((double)v) {}
  Json(long v) : t_(Number), n_((double)v) {}
  Json(unsigned long v) : t_(Number), n_((double)v) {}
  Json(float v) : t_(Number), n_(v) {}
  Json(double v) : t_(Number), n_(v) {}
  Json(const char* v) : t_(String), s_(v) {}
  Json(std::string v) : t_(String), s_(std::move(v)) {}

  static Json array() { Json j; j.t_ = Array; return j; }
  static Json object() { Json j; j.t_ = Object; return j; }
  static Json array(std::initializer_list<Json> items) { Json j = array(); for (auto& i : items) j.a_.push_back(i); return j; }

  Type type() const { return t_; }
  bool is_null() const { return t_ == Null; }
  bool is_bool() const { return t_ == Bool; }
  bool is_number() const { return t_ == Number; }
  bool is_string() const { return t_ == String; }
  bool is_array() const { return t_ == Array; }
  bool is_object() const { return t_ == Object; }
  const char* type_name() const;

  // value access (lenient: returns default on type mismatch)
  bool as_bool(bool def = false) const { return t_ == Bool ? b_ : (t_ == Number ? n_ != 0 : def); }
  double as_num(double def = 0) const { return t_ == Number ? n_ : (t_ == Bool ? (b_ ? 1 : 0) : def); }
  float as_float(float def = 0) const { return (float)as_num(def); }
  int as_int(int def = 0) const { return t_ == Number ? (int)n_ : def; }
  const std::string& as_str() const;
  std::string as_str(const std::string& def) const { return t_ == String ? s_ : def; }

  // arrays
  size_t size() const { return t_ == Array ? a_.size() : (t_ == Object ? o_.size() : 0); }
  const Json& operator[](size_t i) const;
  Json& at(size_t i) { return a_[i]; }
  void push(Json v);
  std::vector<Json>& items() { return a_; }
  const std::vector<Json>& items() const { return a_; }
  void insert(size_t index, Json v);
  void erase_at(size_t index);

  // objects
  const Json* find(const std::string& k) const;
  Json* find(const std::string& k);
  bool has(const std::string& k) const { return find(k) != nullptr; }
  const Json& operator[](const std::string& k) const;  // null if missing
  Json& set(const std::string& k, Json v);            // insert or replace, keeps order
  Json& ref(const std::string& k);                    // insert null if missing
  bool erase(const std::string& k);
  std::vector<std::pair<std::string, Json>>& members() { return o_; }
  const std::vector<std::pair<std::string, Json>>& members() const { return o_; }
  std::vector<std::string> keys() const;

  // convenience getters with defaults
  double num(const std::string& k, double def) const { const Json* v = find(k); return v ? v->as_num(def) : def; }
  float numf(const std::string& k, float def) const { return (float)num(k, def); }
  int integer(const std::string& k, int def) const { const Json* v = find(k); return v && v->is_number() ? (int)v->n_ : def; }
  bool boolean(const std::string& k, bool def) const { const Json* v = find(k); return v ? v->as_bool(def) : def; }
  std::string str(const std::string& k, const std::string& def = "") const { const Json* v = find(k); return v && v->is_string() ? v->s_ : def; }

  // serialization
  std::string dump(int indent = -1) const;
  static Json parse(const std::string& text);  // throws pt::Error with line:col
  static bool try_parse(const std::string& text, Json& out, std::string& err);

  bool operator==(const Json& o) const;
  bool operator!=(const Json& o) const { return !(*this == o); }

  // RFC 7386 merge patch
  void merge_patch(const Json& patch);

 private:
  void dump_to(std::string& out, int indent, int depth) const;
  Type t_ = Null;
  bool b_ = false;
  double n_ = 0;
  std::string s_;
  std::vector<Json> a_;
  std::vector<std::pair<std::string, Json>> o_;
};

}  // namespace pt
