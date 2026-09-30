#include "project.h"

#include <algorithm>
#include <functional>

#include "eval.h"
#include "library.h"

namespace pt {

std::string Project::mesh_path() const {
  std::string m = doc.str("mesh", "");
  if (m.empty()) fail("project '%s' has no \"mesh\"", path.c_str());
  return resolve(m);
}
int Project::resolution() const { return std::clamp(doc.integer("resolution", 2048), 16, 16384); }
int Project::set_resolution(const std::string& set) const {
  const Json& s = doc["texture_sets"][set];
  return std::clamp(s.integer("resolution", resolution()), 16, 16384);
}
BakeSettings Project::bake_settings() const {
  BakeSettings b;
  b.from_json(doc["bake"]);
  return b;
}
float Project::height_depth() const { return doc.numf("height_depth", 0.005f); }
const Json& Project::layers(const std::string& set) const { return doc["texture_sets"][set]["layers"]; }
std::string Project::cache_dir() const { return path_join(dir, ".patina"); }
std::string Project::name() const { return doc.str("name", path_stem(path)); }

Project project_from_doc(const std::string& path, Json doc) {
  Project p;
  p.path = path_abs(path);
  p.dir = path_dir(p.path);
  p.doc = std::move(doc);
  if (!p.doc.is_object()) fail("project file must contain a JSON object");
  return p;
}

Project load_project(const std::string& path) {
  std::string text;
  if (!read_file(path, text)) fail("cannot read project '%s'", path.c_str());
  Json doc;
  std::string err;
  if (!Json::try_parse(text, doc, err)) fail("%s: %s", path.c_str(), err.c_str());
  return project_from_doc(path, std::move(doc));
}

void save_project(const Project& p) { write_file_or_throw(p.path, p.doc.dump(2) + "\n"); }

Json new_project_doc(const std::string& mesh_rel, const Mesh& m, int resolution) {
  Json d = Json::object();
  d.set("patina", 1);
  d.set("mesh", mesh_rel);
  d.set("resolution", resolution);
  d.set("height_depth", 0.005);
  d.set("bake", BakeSettings().to_json());
  Json ex = Json::object();
  ex.set("preset", "blender");
  ex.set("dir", "textures");
  d.set("export", ex);
  Json sets = Json::object();
  for (auto& s : m.set_names) {
    Json sj = Json::object();
    sj.set("layers", Json::array());
    sets.set(s, sj);
  }
  d.set("texture_sets", sets);
  return d;
}

// ---------------------------------------------------------------- layer bookkeeping
// Visits every layer (depth-first, bottom to top). The callback returns true to stop the walk.
static bool walk_layers(Json& arr, const std::string& set, const std::function<bool(Json&, Json&, size_t, const std::string&)>& fn) {
  if (!arr.is_array()) return false;
  for (size_t i = 0; i < arr.size(); i++) {
    Json& L = arr.at(i);
    if (fn(L, arr, i, set)) return true;
    if (L.is_object() && L["layers"].is_array() && walk_layers(L.ref("layers"), set, fn)) return true;
  }
  return false;
}

static void walk_all(Json& doc, const std::function<bool(Json&, Json&, size_t, const std::string&)>& fn) {
  Json* sets = doc.find("texture_sets");
  if (!sets || !sets->is_object()) return;
  for (auto& kv : sets->members())
    if (kv.second.is_object()) {
      Json* layers = kv.second.find("layers");
      if (layers && walk_layers(*layers, kv.first, fn)) return;
    }
}

std::vector<std::string> all_layer_ids(const Json& doc) {
  std::vector<std::string> ids;
  Json copy = doc;
  walk_all(copy, [&](Json& L, Json&, size_t, const std::string&) {
    if (L.is_object() && L["id"].is_string()) ids.push_back(L["id"].as_str());
    return false;
  });
  return ids;
}

static std::string slug(const std::string& s) {
  std::string r;
  for (char c : s) {
    if (std::isalnum((unsigned char)c)) r += (char)std::tolower((unsigned char)c);
    else if (!r.empty() && r.back() != '_') r += '_';
  }
  while (!r.empty() && r.back() == '_') r.pop_back();
  return r.empty() ? "layer" : r.substr(0, 32);
}

static std::string unique_id(const std::string& base, const std::vector<std::string>& taken) {
  if (std::find(taken.begin(), taken.end(), base) == taken.end()) return base;
  for (int i = 2;; i++) {
    std::string c = base + "_" + std::to_string(i);
    if (std::find(taken.begin(), taken.end(), c) == taken.end()) return c;
  }
}

void assign_layer_ids(Json& doc) {
  std::vector<std::string> taken;
  walk_all(doc, [&](Json& L, Json&, size_t, const std::string&) {
    if (!L.is_object()) return false;
    std::string id = L.str("id", "");
    if (id.empty() || std::find(taken.begin(), taken.end(), id) != taken.end()) {
      std::string base = slug(L.str("name", L.str("material", L.str("type", "layer"))));
      id = unique_id(base, taken);
      // keep "id" as the first key for readability
      Json n = Json::object();
      n.set("id", id);
      for (auto& kv : L.members()) if (kv.first != "id") n.set(kv.first, kv.second);
      L = n;
    }
    taken.push_back(id);
    return false;
  });
}

LayerRef find_layer(Json& doc, const std::string& id) {
  LayerRef r;
  walk_all(doc, [&](Json& L, Json& arr, size_t i, const std::string& set) {
    if (L.is_object() && L.str("id", "") == id) {
      r.layer = &L;
      r.container = &arr;
      r.index = i;
      r.set = set;
      return true;
    }
    return false;
  });
  return r;
}

// ---------------------------------------------------------------- edit ops
static Json& set_layers(Json& doc, const std::string& set) {
  Json& sets = doc.ref("texture_sets");
  if (!sets.is_object()) sets = Json::object();
  if (!sets.has(set)) {
    std::string dym = did_you_mean(set, sets.keys());
    fail("unknown texture set '%s'%s; texture sets: %s", set.c_str(), dym.empty() ? "" : (" (did you mean '" + dym + "'?)").c_str(),
         Json([&] { Json a = Json::array(); for (auto& k : sets.keys()) a.push(k); return a; }()).dump().c_str());
  }
  Json& s = sets.ref(set);
  Json& layers = s.ref("layers");
  if (!layers.is_array()) layers = Json::array();
  return layers;
}

static std::string default_set(Json& doc) {
  const Json& sets = doc["texture_sets"];
  if (sets.size() == 1) return sets.members()[0].first;
  fail("this project has %zu texture sets; specify \"set\" (one of %s)", sets.size(), [&] {
    Json a = Json::array();
    for (auto& k : sets.keys()) a.push(k);
    return a.dump();
  }().c_str());
}

// Resolve where to insert: {"index": n} | {"before": id} | {"after": id} | {"parent": folder_id}; default = top of the set.
static void insert_layer(Json& doc, const Json& op, Json layer) {
  if (const Json* rel = op.find("before"); rel || op.has("after")) {
    std::string rid = rel ? rel->as_str() : op.str("after");
    LayerRef r = find_layer(doc, rid);
    if (!r.layer) fail("layer '%s' not found", rid.c_str());
    r.container->insert(r.index + (rel ? 0 : 1), std::move(layer));
    return;
  }
  Json* target;
  if (op.has("parent")) {
    LayerRef r = find_layer(doc, op.str("parent"));
    if (!r.layer) fail("parent layer '%s' not found", op.str("parent").c_str());
    Json& ch = r.layer->ref("layers");
    if (!ch.is_array()) ch = Json::array();
    target = &ch;
  } else {
    std::string set = op.str("set", "");
    if (set.empty()) set = default_set(doc);
    target = &set_layers(doc, set);
  }
  if (op.has("index")) {
    int idx = op.integer("index", -1);
    if (idx < 0) idx = (int)target->size() + 1 + idx;
    target->insert((size_t)std::clamp(idx, 0, (int)target->size()), std::move(layer));
  } else {
    target->push(std::move(layer));
  }
}

static Json* resolve_pointer(Json& doc, const std::string& ptr, bool create) {
  if (ptr.empty() || ptr == "/") return &doc;
  if (ptr[0] != '/') fail("path must be a JSON pointer like /resolution or /texture_sets/Crate/resolution");
  Json* cur = &doc;
  size_t i = 1;
  while (i <= ptr.size()) {
    size_t j = ptr.find('/', i);
    if (j == std::string::npos) j = ptr.size();
    std::string tok = ptr.substr(i, j - i);
    size_t p;
    while ((p = tok.find("~1")) != std::string::npos) tok.replace(p, 2, "/");
    while ((p = tok.find("~0")) != std::string::npos) tok.replace(p, 2, "~");
    if (cur->is_array()) {
      size_t idx = (size_t)std::atoi(tok.c_str());
      if (idx >= cur->size()) fail("index %zu out of range in '%s'", idx, ptr.c_str());
      cur = &cur->at(idx);
    } else {
      if (!cur->is_object()) { if (!create) fail("'%s' does not exist", ptr.c_str()); *cur = Json::object(); }
      Json* nx = cur->find(tok);
      if (!nx) { if (!create) fail("'%s' does not exist", ptr.c_str()); nx = &cur->ref(tok); }
      cur = nx;
    }
    i = j + 1;
  }
  return cur;
}

Json apply_edit_ops(Json& doc, const Json& ops_in) {
  Json ops = ops_in.is_array() ? ops_in : Json::array({ops_in});
  Json results = Json::array();
  for (size_t k = 0; k < ops.size(); k++) {
    const Json& op = ops[k];
    std::string kind = op.str("op", "");
    try {
      if (kind == "add") {
        Json layer = op["layer"];
        if (!layer.is_object()) fail("add needs \"layer\": {...}");
        std::vector<std::string> taken = all_layer_ids(doc);
        std::string id = layer.str("id", "");
        if (id.empty() || std::find(taken.begin(), taken.end(), id) != taken.end()) {
          std::string nid = unique_id(id.empty() ? slug(layer.str("name", layer.str("material", layer.str("type", "layer")))) : id, taken);
          Json n = Json::object();
          n.set("id", nid);
          for (auto& kv : layer.members()) if (kv.first != "id") n.set(kv.first, kv.second);
          layer = n;
          id = nid;
        }
        insert_layer(doc, op, layer);
        results.push(strf("added layer '%s'", id.c_str()));
      } else if (kind == "update" || kind == "patch") {
        std::string id = op.str("id", "");
        LayerRef r = find_layer(doc, id);
        if (!r.layer) {
          std::string dym = did_you_mean(id, all_layer_ids(doc));
          fail("layer '%s' not found%s", id.c_str(), dym.empty() ? "" : (" (did you mean '" + dym + "'?)").c_str());
        }
        const Json& patch = op.has("patch") ? op["patch"] : op["layer"];
        if (!patch.is_object()) fail("update needs \"patch\": {...} (JSON merge patch; null deletes a key)");
        r.layer->merge_patch(patch);
        results.push(strf("updated layer '%s'", id.c_str()));
      } else if (kind == "remove" || kind == "delete") {
        std::string id = op.str("id", "");
        LayerRef r = find_layer(doc, id);
        if (!r.layer) fail("layer '%s' not found", id.c_str());
        r.container->erase_at(r.index);
        results.push(strf("removed layer '%s'", id.c_str()));
      } else if (kind == "move") {
        std::string id = op.str("id", "");
        LayerRef r = find_layer(doc, id);
        if (!r.layer) fail("layer '%s' not found", id.c_str());
        Json layer = *r.layer;
        r.container->erase_at(r.index);
        Json where = op;
        if (!where.has("set") && !where.has("parent") && !where.has("before") && !where.has("after")) where.set("set", r.set);
        insert_layer(doc, where, layer);
        results.push(strf("moved layer '%s'", id.c_str()));
      } else if (kind == "duplicate") {
        std::string id = op.str("id", "");
        LayerRef r = find_layer(doc, id);
        if (!r.layer) fail("layer '%s' not found", id.c_str());
        Json layer = *r.layer;
        std::string nid = unique_id(op.str("new_id", id + "_copy"), all_layer_ids(doc));
        layer.set("id", nid);
        if (op.has("patch")) layer.merge_patch(op["patch"]);
        r.container->insert(r.index + 1, layer);
        results.push(strf("duplicated '%s' as '%s'", id.c_str(), nid.c_str()));
      } else if (kind == "replace" || kind == "set_layers") {
        std::string set = op.str("set", "");
        if (set.empty()) set = default_set(doc);
        Json& layers = set_layers(doc, set);
        if (!op["layers"].is_array()) fail("replace needs \"layers\": [...]");
        layers = op["layers"];
        results.push(strf("replaced layers of '%s'", set.c_str()));
      } else if (kind == "set") {
        std::string ptr = op.str("path", "");
        *resolve_pointer(doc, ptr, true) = op["value"];
        results.push(strf("set %s", ptr.c_str()));
      } else {
        fail("unknown op '%s' (add, update, remove, move, duplicate, replace, set)", kind.c_str());
      }
    } catch (const Error& e) {
      throw Error(strf("op %zu (%s): %s", k, kind.c_str(), e.what()));
    }
  }
  assign_layer_ids(doc);
  return results;
}

// ---------------------------------------------------------------- validation & outline
Json validate_project(const Project& p, const Mesh* m) {
  Json errors = Json::array(), warnings = Json::array();
  static const char* top_keys[] = {"patina", "name", "mesh", "resolution", "height_depth", "bake", "export", "texture_sets", "comment", "note", "render"};
  for (auto& kv : p.doc.members()) {
    bool ok = false;
    for (auto* k : top_keys) if (kv.first == k) ok = true;
    if (!ok) {
      std::vector<std::string> c(std::begin(top_keys), std::end(top_keys));
      std::string dym = did_you_mean(kv.first, c);
      warnings.push(strf("unknown top-level key '%s'%s", kv.first.c_str(), dym.empty() ? "" : (" (did you mean '" + dym + "'?)").c_str()));
    }
  }
  if (!p.doc["mesh"].is_string()) errors.push("\"mesh\" must be a path to a .glb/.gltf/.obj file");
  else if (!file_exists(p.mesh_path())) errors.push(strf("mesh not found: %s", p.mesh_path().c_str()));
  if (!p.doc["texture_sets"].is_object()) errors.push("\"texture_sets\" must be an object keyed by material name");
  if (m) {
    for (auto& kv : p.doc["texture_sets"].members())
      if (m->find_set(kv.first) < 0) {
        std::string dym = did_you_mean(kv.first, m->set_names);
        warnings.push(strf("texture set '%s' does not match any material in the mesh%s", kv.first.c_str(), dym.empty() ? "" : (" (did you mean '" + dym + "'?)").c_str()));
      }
    for (auto& s : m->set_names)
      if (!p.doc["texture_sets"].has(s)) warnings.push(strf("material '%s' has no texture set entry (it will use default gray)", s.c_str()));
  }
  std::vector<std::string> ids = all_layer_ids(p.doc);
  std::vector<std::string> sorted = ids;
  std::sort(sorted.begin(), sorted.end());
  for (size_t i = 1; i < sorted.size(); i++)
    if (sorted[i] == sorted[i - 1]) errors.push(strf("duplicate layer id '%s'", sorted[i].c_str()));
  Json r = Json::object();
  r.set("ok", errors.size() == 0);
  r.set("errors", errors);
  r.set("warnings", warnings);
  return r;
}

static Json outline_layers(const Json& layers) {
  Json out = Json::array();
  for (auto& L : layers.items()) {
    if (!L.is_object()) continue;
    std::string type = L.str("type", L.has("layers") ? "folder" : (L.has("material") ? "smart" : "fill"));
    std::string line = strf("%s [%s]", L.str("id", "?").c_str(), type.c_str());
    if (type == "smart") line += " " + L.str("material", "");
    if (!L.boolean("enabled", true)) line += " (disabled)";
    if (L.has("opacity")) line += strf(" opacity=%.2f", L.numf("opacity", 1));
    if (L["channels"].is_object()) {
      line += " channels:";
      for (auto& kv : L["channels"].members()) line += " " + kv.first;
    }
    if (L["mask"].is_array() || L["mask"].is_object()) {
      line += " mask:";
      const Json& mk = L["mask"];
      auto add = [&](const Json& e) { line += " " + e.str("type", "?"); };
      if (mk.is_array()) for (auto& e : mk.items()) add(e);
      else add(mk);
    }
    if (type == "folder" && L["layers"].is_array()) {
      Json o = Json::object();
      o.set("layer", line);
      o.set("children", outline_layers(L["layers"]));
      out.push(o);
    } else {
      out.push(line);
    }
  }
  return out;
}

Json project_outline(const Project& p) {
  Json o = Json::object();
  o.set("project", p.path);
  o.set("mesh", p.doc.str("mesh", ""));
  o.set("resolution", p.resolution());
  Json sets = Json::object();
  for (auto& kv : p.doc["texture_sets"].members()) sets.set(kv.first, outline_layers(kv.second["layers"]));
  o.set("texture_sets (layers bottom -> top)", sets);
  return o;
}

}  // namespace pt
