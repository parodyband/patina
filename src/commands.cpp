#include "commands.h"

#include <algorithm>
#include <cstdio>
#include <future>
#include <map>
#include <mutex>
#include <thread>

#include "bake.h"
#include "eval.h"
#include "export.h"
#include "image.h"
#include "library.h"
#include "mesh.h"
#include "project.h"
#include "render.h"

namespace pt {

// ---------------------------------------------------------------- session caches (shared by concurrent requests)
namespace {
struct MeshEntry { int64_t mtime; std::shared_ptr<const Mesh> mesh; };
std::mutex g_cache_m;
std::map<std::string, MeshEntry> g_meshes;
std::map<std::string, std::shared_future<std::shared_ptr<Baked>>> g_bakes;
std::vector<std::string> g_bake_order;  // LRU-ish eviction
std::map<std::string, std::shared_ptr<std::mutex>> g_project_locks;

std::shared_ptr<const Mesh> get_mesh(const std::string& path) {
  std::string key = path_abs(path);
  int64_t mt = file_mtime_ns(key);
  {
    std::lock_guard<std::mutex> lk(g_cache_m);
    auto it = g_meshes.find(key);
    if (it != g_meshes.end() && it->second.mtime == mt) return it->second.mesh;
  }
  auto m = std::make_shared<const Mesh>(load_mesh(key));
  std::lock_guard<std::mutex> lk(g_cache_m);
  g_meshes[key] = {mt, m};
  return m;
}

std::shared_ptr<Baked> get_bake(const Project& p, int res_override, bool force, Json* stats_out) {
  auto mesh = get_mesh(p.mesh_path());
  std::vector<int> res;
  for (auto& s : mesh->set_names) res.push_back(res_override > 0 ? std::min(res_override, p.set_resolution(s)) : p.set_resolution(s));
  BakeSettings bs = p.bake_settings();
  std::string key = strf("%s|%llx|%llx|", mesh->path.c_str(), (unsigned long long)mesh->content_hash, (unsigned long long)bs.hash());
  for (int r : res) key += strf("%d,", r);
  std::promise<std::shared_ptr<Baked>> promise;
  std::shared_future<std::shared_ptr<Baked>> fut;
  bool owner = false;
  {
    std::lock_guard<std::mutex> lk(g_cache_m);
    auto it = g_bakes.find(key);
    if (it != g_bakes.end() && !force) fut = it->second;
    else {
      fut = promise.get_future().share();
      g_bakes[key] = fut;
      g_bake_order.push_back(key);
      owner = true;
      while (g_bake_order.size() > 12) {
        g_bakes.erase(g_bake_order.front());
        g_bake_order.erase(g_bake_order.begin());
      }
    }
  }
  if (owner) {
    try {
      promise.set_value(bake_mesh(mesh, res, bs, p.cache_dir(), force));
    } catch (...) {
      promise.set_exception(std::current_exception());
      std::lock_guard<std::mutex> lk(g_cache_m);
      g_bakes.erase(key);
    }
  }
  auto bk = fut.get();
  if (stats_out) {
    *stats_out = bk->stats;
    stats_out->set("session_cache_hit", !owner);
  }
  return bk;
}

std::shared_ptr<std::mutex> project_lock(const std::string& path) {
  std::lock_guard<std::mutex> lk(g_cache_m);
  auto& m = g_project_locks[path_abs(path)];
  if (!m) m = std::make_shared<std::mutex>();
  return m;
}

// ---------------------------------------------------------------- argument helpers
std::string req_str(const Json& a, const char* key) {
  const Json* v = a.find(key);
  if (!v || !v->is_string() || v->as_str().empty()) fail("missing required argument \"%s\"", key);
  return v->as_str();
}

Project load_project_arg(const Json& a) {
  std::string path = req_str(a, "project");
  Project p = load_project(path);
  if (const Json* inline_doc = a.find("doc"); inline_doc && inline_doc->is_object()) p.doc = *inline_doc;
  return p;
}

std::vector<SetResult> evaluate_all(const Project& p, const Baked& bk, const EvalOptions& opt, Json* stats, Json* warnings) {
  std::vector<SetResult> results(bk.sets.size());
  // texture sets evaluate concurrently; each also parallelizes internally
  std::mutex err_m;
  std::exception_ptr err;
  parallel_for((int64_t)bk.sets.size(), 1, [&](int64_t b, int64_t e) {
    for (int64_t i = b; i < e; i++) {
      try {
        results[i] = evaluate_set(p, bk, (int)i, opt);
      } catch (...) {
        std::lock_guard<std::mutex> lk(err_m);
        if (!err) err = std::current_exception();
      }
    }
  });
  if (err) std::rethrow_exception(err);
  if (stats) {
    *stats = Json::object();
    for (auto& r : results) stats->set(r.name, r.stats);
  }
  if (warnings) {
    *warnings = Json::array();
    for (auto& r : results) for (auto& w : r.warnings) warnings->push(w);
  }
  return results;
}

std::string default_render_path(const Project& p, const std::string& suffix) {
  return path_join(path_join(p.dir, "renders"), sanitize_filename(p.name()) + suffix + ".png");
}

// ---------------------------------------------------------------- commands
CommandOutput cmd_inspect(const Json& a) {
  std::string mesh = a.has("mesh") ? req_str(a, "mesh") : "";
  if (mesh.empty()) mesh = load_project_arg(a).mesh_path();
  auto m = get_mesh(mesh);
  CommandOutput o;
  o.result = mesh_info(*m);
  return o;
}

CommandOutput cmd_new(const Json& a) {
  std::string path = req_str(a, "project");
  std::string mesh = req_str(a, "mesh");
  if (file_exists(path) && !a.boolean("overwrite", false)) fail("project '%s' already exists (pass overwrite: true to replace it)", path.c_str());
  auto m = get_mesh(mesh);
  std::string pdir = path_dir(path_abs(path));
  make_dirs(pdir);
  Json doc = new_project_doc(path_relative(mesh, pdir), *m, std::clamp(a.integer("resolution", 2048), 16, 16384));
  if (a.has("name")) doc.set("name", a["name"]);
  if (a.has("preset")) doc.ref("export").set("preset", a["preset"]);
  // optional starting materials: "smart": "painted_metal" | {"material":..,"params":..} | {"SetName": {...}, ...}
  if (const Json* sm = a.find("smart")) {
    for (auto& set : m->set_names) {
      Json spec;
      if (sm->is_string()) { spec = Json::object(); spec.set("material", *sm); }
      else if (sm->is_object() && sm->has("material")) spec = *sm;
      else if (sm->is_object() && sm->has(set)) spec = (*sm)[set].is_string() ? Json(Json::object()) : (*sm)[set];
      if (sm->is_object() && (*sm)[set].is_string()) spec.set("material", (*sm)[set]);
      if (!spec.is_object() || !spec.has("material")) continue;
      Json layer = Json::object();
      layer.set("id", sanitize_filename(to_lower(spec.str("material"))));
      layer.set("type", "smart");
      layer.set("material", spec["material"]);
      if (spec.has("params")) layer.set("params", spec["params"]);
      doc.ref("texture_sets").ref(set).ref("layers").push(layer);
    }
  }
  assign_layer_ids(doc);
  Project p = project_from_doc(path, doc);
  save_project(p);
  CommandOutput o;
  o.result = Json::object();
  o.result.set("project", p.path);
  o.result.set("texture_sets", [&] { Json j = Json::array(); for (auto& s : m->set_names) j.push(s); return j; }());
  o.result.set("mesh", mesh_info(*m));
  o.result.set("next", "add layers with `edit` (or edit the JSON), then `render` to look at the result and `export` when done");
  return o;
}

CommandOutput cmd_get(const Json& a) {
  Project p = load_project_arg(a);
  CommandOutput o;
  if (a.boolean("outline", false)) o.result = project_outline(p);
  else o.result = p.doc;
  return o;
}

CommandOutput cmd_edit(const Json& a) {
  std::string path = req_str(a, "project");
  auto lock = project_lock(path);
  std::lock_guard<std::mutex> lk(*lock);
  Project p = load_project(path);
  Json doc = p.doc;
  Json ops = a["ops"];
  if (ops.is_null()) fail("edit needs \"ops\": [{\"op\":\"add\",...}]");
  Json results = apply_edit_ops(doc, ops);
  p.doc = doc;
  // validate by evaluating at low resolution before saving (fail fast, keep the file valid)
  Json warnings = Json::array();
  if (!a.boolean("skip_check", false)) {
    auto bk = get_bake(p, std::min(256, p.resolution()), false, nullptr);
    EvalOptions opt;
    evaluate_all(p, *bk, opt, nullptr, &warnings);
  }
  save_project(p);
  CommandOutput o;
  o.result = Json::object();
  o.result.set("ok", true);
  o.result.set("applied", results);
  o.result.set("warnings", warnings);
  o.result.set("outline", project_outline(p));
  return o;
}

CommandOutput cmd_validate(const Json& a) {
  Project p = load_project_arg(a);
  std::shared_ptr<const Mesh> m;
  try { m = get_mesh(p.mesh_path()); } catch (const std::exception&) {}
  Json r = validate_project(p, m.get());
  if (m && r.boolean("ok", false)) {
    try {
      auto bk = get_bake(p, std::min(256, p.resolution()), false, nullptr);
      Json warnings;
      EvalOptions opt;
      evaluate_all(p, *bk, opt, nullptr, &warnings);
      for (auto& w : warnings.items()) r.ref("warnings").push(w);
    } catch (const std::exception& e) {
      r.set("ok", false);
      r.ref("errors").push(e.what());
    }
  }
  CommandOutput o;
  o.result = r;
  return o;
}

CommandOutput cmd_bake(const Json& a) {
  Project p = load_project_arg(a);
  Json stats;
  int res = a.integer("resolution", 0);
  auto bk = get_bake(p, res, a.boolean("force", false), &stats);
  CommandOutput o;
  o.result = Json::object();
  o.result.set("stats", stats);
  if (a.has("dump") || a.boolean("preview", false)) {
    // baked maps as images (and a contact sheet agents can look at)
    std::string dir = a.str("dump", path_join(p.dir, "renders"));
    make_dirs(dir);
    std::vector<RgbImage> tiles;
    std::vector<std::string> labels;
    for (auto& ss : bk->sets) {
      std::vector<float> img((size_t)ss.res * ss.res);
      struct { const char* name; const std::vector<float>* v; } maps[] = {{"ao", &ss.ao}, {"curvature", &ss.curvature}, {"thickness", &ss.thickness}};
      for (auto& mp : maps) {
        to_image(ss, mp.v->data(), 1, img.data());
        std::string f = path_join(dir, sanitize_filename(ss.name) + "_" + mp.name + ".png");
        save_png(f, ss.res, ss.res, 1, img.data(), false, 8);
        RgbImage t;
        int th = 256;
        t.w = t.h = th;
        t.px.resize((size_t)th * th * 3);
        for (int y = 0; y < th; y++)
          for (int x = 0; x < th; x++) {
            float v = img[(size_t)(y * ss.res / th) * ss.res + (x * ss.res / th)];
            uint8_t b = (uint8_t)std::lround(saturate(v) * 255);
            t.px[((size_t)y * th + x) * 3] = t.px[((size_t)y * th + x) * 3 + 1] = t.px[((size_t)y * th + x) * 3 + 2] = b;
          }
        tiles.push_back(t);
        labels.push_back(ss.name + " " + mp.name);
      }
    }
    RgbImage sheet = compose_grid(tiles, labels, 3, "baked maps");
    std::string png = encode_png_rgb8(sheet.w, sheet.h, sheet.px.data());
    std::string sp = path_join(dir, sanitize_filename(p.name()) + "_bake.png");
    write_file_or_throw(sp, png);
    o.result.set("sheet", sp);
    if (a.boolean("return_image", true)) o.images.push_back(png);
  }
  return o;
}

RenderOptions parse_render_options(const Json& a) {
  RenderOptions ro;
  ro.size = std::clamp(a.integer("size", 512), 64, 4096);
  ro.ssaa = std::clamp(a.integer("ssaa", 2), 1, 4);
  ro.mode = a.str("mode", "lit");
  ro.shadows = a.boolean("shadows", true);
  ro.labels = a.boolean("labels", true);
  ro.columns = a.integer("columns", 0);
  ro.exposure = a.numf("exposure", 1.f);
  return ro;
}

std::vector<std::string> extra_maps_for_mode(const std::string& mode) {
  std::vector<std::string> e;
  if (mode.rfind("mask:", 0) == 0) e.push_back(mode);
  if (mode == "curvature" || mode == "thickness" || mode == "bake_ao") e.push_back(mode);
  return e;
}

CommandOutput cmd_render(const Json& a) {
  Timer total;
  Project p = load_project_arg(a);
  RenderOptions ro = parse_render_options(a);
  int res = a.integer("resolution", std::min(1024, p.resolution()));
  Json bake_stats;
  auto bk = get_bake(p, res, false, &bake_stats);
  EvalOptions opt;
  if (ro.mode.rfind("mask:", 0) == 0) {
    std::string id = ro.mode.substr(5);
    opt.record_masks.insert(id);
    Json doc = p.doc;
    if (!find_layer(doc, id).layer && id.find('/') == std::string::npos) {
      std::string dym = did_you_mean(id, all_layer_ids(doc));
      fail("mask view: layer '%s' not found%s", id.c_str(), dym.empty() ? "" : (" (did you mean '" + dym + "'?)").c_str());
    }
  }
  Json eval_stats, warnings;
  Timer te;
  auto results = evaluate_all(p, *bk, opt, &eval_stats, &warnings);
  double eval_ms = te.ms();
  std::vector<SetMaps> maps(bk->sets.size());
  std::vector<std::string> extra = extra_maps_for_mode(ro.mode);
  for (size_t i = 0; i < results.size(); i++) maps[i] = make_maps(bk->sets[i], results[i], extra);

  Timer tr;
  RgbImage img;
  bool sheet = a.boolean("sheet", false) || ro.mode == "sheet";
  if (sheet) {
    img = render_texture_sheet(maps, std::clamp(a.integer("size", 256), 64, 1024), p.name() + " texture maps");
  } else {
    auto views = parse_views(a["views"]);
    ro.title = strf("%s  [%s]", p.name().c_str(), ro.mode.c_str());
    img = render_views(*bk->mesh, bk->bvh, maps, views, ro);
  }
  double render_ms = tr.ms();
  std::string png = encode_png_rgb8(img.w, img.h, img.px.data());
  std::string out = a.str("out", default_render_path(p, sheet ? "_sheet" : (ro.mode == "lit" ? "" : "_" + sanitize_filename(ro.mode))));
  make_dirs(path_dir(path_abs(out)));
  write_file_or_throw(out, png);

  CommandOutput o;
  o.result = Json::object();
  o.result.set("image", path_abs(out));
  o.result.set("width", img.w);
  o.result.set("height", img.h);
  o.result.set("warnings", warnings);
  Json t = Json::object();
  t.set("bake_ms", bake_stats.num("total_ms", 0));
  t.set("bake_cached", bake_stats.boolean("session_cache_hit", false) || bake_stats.boolean("from_cache", false));
  t.set("eval_ms", eval_ms);
  t.set("render_ms", render_ms);
  t.set("total_ms", total.ms());
  t.set("texture_resolution", res);
  o.result.set("timing", t);
  if (a.boolean("stats", false)) o.result.set("eval_stats", eval_stats);
  if (a.boolean("return_image", true)) o.images.push_back(png);
  return o;
}

// Evaluate several edited copies of a project in parallel and show them side by side.
CommandOutput cmd_variants(const Json& a) {
  Timer total;
  Project base = load_project_arg(a);
  const Json& variants = a["variants"];
  if (!variants.is_array() || variants.size() == 0) fail("variants needs \"variants\": [{\"label\":\"...\",\"ops\":[...]}, ...]");
  if (variants.size() > 16) fail("at most 16 variants per call");
  RenderOptions ro = parse_render_options(a);
  if (!a.has("size")) ro.size = 384;
  int res = a.integer("resolution", std::min(1024, base.resolution()));
  auto bk = get_bake(base, res, false, nullptr);
  auto views = parse_views(a.has("views") ? a["views"] : Json("iso"));
  size_t nv = variants.size();
  std::vector<std::vector<RgbImage>> tiles(nv);
  std::vector<std::string> labels(nv), errors(nv);
  Json docs = Json::array();
  for (size_t i = 0; i < nv; i++) docs.push(Json());
  parallel_for((int64_t)nv, 1, [&](int64_t b, int64_t e) {
    for (int64_t i = b; i < e; i++) {
      const Json& v = variants[i];
      labels[i] = v.str("label", strf("variant %lld", (long long)i));
      try {
        Project p = base;
        if (v.has("ops")) apply_edit_ops(p.doc, v["ops"]);
        if (v.has("doc")) p.doc = v["doc"];
        EvalOptions opt;
        if (ro.mode.rfind("mask:", 0) == 0) opt.record_masks.insert(ro.mode.substr(5));
        auto results = evaluate_all(p, *bk, opt, nullptr, nullptr);
        std::vector<SetMaps> maps(bk->sets.size());
        for (size_t s = 0; s < results.size(); s++) maps[s] = make_maps(bk->sets[s], results[s], extra_maps_for_mode(ro.mode));
        RenderOptions r2 = ro;
        r2.labels = false;
        for (auto& view : views) tiles[i].push_back(render_view(*bk->mesh, bk->bvh, maps, view, r2));
      } catch (const std::exception& ex) {
        errors[i] = ex.what();
      }
    }
  });
  std::vector<RgbImage> flat;
  std::vector<std::string> flat_labels;
  for (size_t i = 0; i < nv; i++) {
    for (size_t k = 0; k < views.size(); k++) {
      if (tiles[i].size() > k) flat.push_back(tiles[i][k]);
      else { RgbImage blank; blank.w = blank.h = ro.size; blank.px.assign((size_t)ro.size * ro.size * 3, 60); flat.push_back(blank); }
      flat_labels.push_back(k == 0 ? strf("%zu: %s", i, labels[i].c_str()) + (errors[i].empty() ? "" : " (ERROR)") : "");
    }
  }
  int cols = a.integer("columns", views.size() > 1 ? (int)views.size() : std::min<int>((int)nv, 4));
  RgbImage img = compose_grid(flat, flat_labels, cols, base.name() + " variants");
  std::string png = encode_png_rgb8(img.w, img.h, img.px.data());
  std::string out = a.str("out", default_render_path(base, "_variants"));
  make_dirs(path_dir(path_abs(out)));
  write_file_or_throw(out, png);
  CommandOutput o;
  o.result = Json::object();
  o.result.set("image", path_abs(out));
  Json vr = Json::array();
  for (size_t i = 0; i < nv; i++) {
    Json e = Json::object();
    e.set("index", (int)i);
    e.set("label", labels[i]);
    if (!errors[i].empty()) e.set("error", errors[i]);
    vr.push(e);
  }
  o.result.set("variants", vr);
  o.result.set("hint", "apply a winner with `edit` using that variant's ops");
  o.result.set("total_ms", total.ms());
  if (a.boolean("return_image", true)) o.images.push_back(png);
  return o;
}

CommandOutput cmd_export(const Json& a) {
  Timer total;
  Project p = load_project_arg(a);
  std::string preset = a.str("preset", p.doc["export"].str("preset", "blender"));
  std::string out = a.str("out", p.resolve(p.doc["export"].str("dir", "textures")));
  int res = a.integer("resolution", 0);
  Json bake_stats;
  auto bk = get_bake(p, res, false, &bake_stats);
  EvalOptions opt;
  Json eval_stats, warnings;
  auto results = evaluate_all(p, *bk, opt, &eval_stats, &warnings);
  Json r = export_textures(p, *bk, results, preset, out, a.boolean("glb", false));
  r.set("warnings", warnings);
  r.set("bake_ms", bake_stats.num("total_ms", 0));
  r.set("eval", eval_stats);
  r.set("total_ms", total.ms());
  CommandOutput o;
  o.result = r;
  return o;
}

CommandOutput cmd_library(const Json& a) {
  std::string dir = a.has("project") ? path_dir(path_abs(a.str("project"))) : "";
  std::string topic = a.str("topic", "");
  CommandOutput o;
  Json j = Json::object();
  if (topic.empty() || topic == "smart_materials") j.set("smart_materials", smart_material_catalog(dir));
  if (topic.empty() || topic == "fields") j.set("fields (use in masks and as channel values)", field_catalog());
  if (topic.empty() || topic == "channels") {
    Json ch = Json::object();
    for (auto& c : kChannels) ch.set(c.name, c.doc);
    j.set("channels", ch);
  }
  if (topic.empty() || topic == "blend_modes") j.set("blend_modes", blend_mode_list());
  if (topic.empty() || topic == "export_presets") j.set("export_presets", export_preset_catalog());
  if (topic.empty() || topic == "render") {
    j.set("render_modes", render_modes());
    Json v = Json::array();
    for (auto& n : view_names()) v.push(n);
    j.set("views", v);
  }
  if (topic.empty() || topic == "modifiers") {
    Json m = Json::object();
    m.set("order", "blur -> levels -> contrast -> power -> invert -> threshold -> multiply -> add -> clamp");
    m.set("blur", "radius in texels at 1024 (scaled with resolution)");
    m.set("levels", "[in_lo, in_hi] or {\"in\":[lo,hi],\"gamma\":g,\"out\":[lo,hi]}");
    m.set("contrast", "-1..1");
    m.set("power", "exponent");
    m.set("invert", "true/false");
    m.set("threshold", "value or {\"value\":0.5,\"softness\":0.02}");
    m.set("multiply / add", "scalar arithmetic");
    m.set("clamp", "true (default) keeps 0..1");
    m.set("range", "channel values only: [lo, hi] maps the 0..1 field onto a value range (e.g. roughness [0.3,0.6], height [-0.2,0.2])");
    m.set("gradient", "color channels only: [\"#hex\",...] or [[pos,\"#hex\"],...] maps the field to colors");
    j.set("field_modifiers", m);
  }
  o.result = j;
  return o;
}

// ---------------------------------------------------------------- blender bridge
std::string find_blender() {
  if (const char* e = std::getenv("PATINA_BLENDER")) if (file_exists(e)) return e;
#if defined(__APPLE__)
  if (file_exists("/Applications/Blender.app/Contents/MacOS/Blender")) return "/Applications/Blender.app/Contents/MacOS/Blender";
#elif defined(_WIN32)
  std::string root = "C:/Program Files/Blender Foundation";
  if (is_directory(root)) {
    std::error_code ec;
    std::vector<std::string> cands;
    for (auto& f : list_dir(root)) (void)f;
    // list_dir only returns files; probe common versioned folders instead
    for (int major = 6; major >= 3; major--)
      for (int minor = 9; minor >= 0; minor--) {
        std::string c = strf("%s/Blender %d.%d/blender.exe", root.c_str(), major, minor);
        if (file_exists(c)) return c;
      }
  }
#endif
  return "blender";
}

std::string find_bridge_script() {
  if (const char* e = std::getenv("PATINA_BLENDER_SCRIPT")) if (file_exists(e)) return e;
  std::string dir = path_dir(exe_path());
  for (const char* rel : {"tools/blender/patina_blender.py", "../tools/blender/patina_blender.py", "../../tools/blender/patina_blender.py"}) {
    std::string c = path_join(dir, rel);
    if (file_exists(c)) return path_abs(c);
  }
  fail("cannot find tools/blender/patina_blender.py (set PATINA_BLENDER_SCRIPT)");
}

std::string shell_quote(const std::string& s) {
#if defined(_WIN32)
  return "\"" + s + "\"";
#else
  std::string r = "'";
  for (char c : s) { if (c == '\'') r += "'\\''"; else r += c; }
  return r + "'";
#endif
}

CommandOutput cmd_blender(const Json& a) {
  std::string action = req_str(a, "action");
  std::string blender = find_blender();
  std::string script = find_bridge_script();
  std::vector<std::string> args = {blender, "-b", "--factory-startup", "--python", script, "--", action};
  auto opt = [&](const char* key, const char* flag) {
    if (a.has(key)) {
      const Json& v = a[key];
      if (v.is_bool()) { if (v.as_bool()) args.push_back(flag); }
      else { args.push_back(flag); args.push_back(v.is_string() ? v.as_str() : v.dump()); }
    }
  };
  if (action == "export") {
    opt("blend", "--blend"); opt("out", "--out"); opt("objects", "--objects"); opt("auto_uv", "--auto-uv");
  } else if (action == "apply") {
    std::string manifest = a.str("manifest", "");
    if (manifest.empty() && a.has("project")) {
      Project p = load_project_arg(a);
      manifest = path_join(p.resolve(p.doc["export"].str("dir", "textures")), "manifest.json");
    }
    if (manifest.empty()) fail("apply needs \"manifest\" (from export) or \"project\"");
    args.push_back("--manifest"); args.push_back(manifest);
    opt("mesh", "--mesh"); opt("blend", "--blend"); opt("out", "--out"); opt("render", "--render"); opt("engine", "--engine");
    opt("samples", "--samples"); opt("res", "--res");
  } else {
    fail("blender action must be export or apply");
  }
  std::string cmd;
  for (auto& s : args) { if (!cmd.empty()) cmd += ' '; cmd += shell_quote(s); }
  cmd += " 2>&1";
#if defined(_WIN32)
  FILE* f = _popen(("\"" + cmd + "\"").c_str(), "r");
#else
  FILE* f = popen(cmd.c_str(), "r");
#endif
  if (!f) fail("cannot run blender (%s)", blender.c_str());
  std::string output, line;
  char buf[4096];
  Json parsed;
  while (fgets(buf, sizeof buf, f)) {
    output += buf;
    std::string l = buf;
    if (l.rfind("PATINA_JSON ", 0) == 0) {
      std::string err;
      Json::try_parse(l.substr(12), parsed, err);
    }
  }
#if defined(_WIN32)
  int status = _pclose(f);
#else
  int status = pclose(f);
#endif
  CommandOutput o;
  o.result = parsed.is_object() ? parsed : Json::object();
  o.result.set("exit_status", status);
  if (status != 0 || !parsed.is_object()) {
    std::string tail = output.size() > 3000 ? output.substr(output.size() - 3000) : output;
    fail("blender %s failed (status %d):\n%s", action.c_str(), status, tail.c_str());
  }
  return o;
}

// ---------------------------------------------------------------- batch
CommandOutput cmd_batch(const Json& a) {
  const Json& jobs = a["jobs"];
  if (!jobs.is_array()) fail("batch needs \"jobs\": [{\"command\":\"render\",\"args\":{...}}, ...]");
  Timer total;
  size_t n = jobs.size();
  std::vector<Json> results(n);
  std::vector<std::string> images;
  std::mutex img_m;
  int parallel = std::clamp(a.integer("parallel", 4), 1, 64);
  std::atomic<size_t> next{0};
  std::vector<std::thread> workers;
  for (int w = 0; w < std::min<int>(parallel, (int)n); w++)
    workers.emplace_back([&] {
      for (;;) {
        size_t i = next.fetch_add(1);
        if (i >= n) return;
        const Json& job = jobs[i];
        Json r = Json::object();
        r.set("index", (int64_t)i);
        std::string name = job.str("command", "");
        r.set("command", name);
        Timer t;
        try {
          if (name == "batch") fail("nested batch is not allowed");
          Json args = job["args"];
          if (args.is_object()) args.set("return_image", args.boolean("return_image", false));
          CommandOutput co = run_command(name, args);
          r.set("ok", true);
          r.set("result", co.result);
          if (!co.images.empty() && a.boolean("return_images", false)) {
            std::lock_guard<std::mutex> lk(img_m);
            for (auto& im : co.images) images.push_back(im);
          }
        } catch (const std::exception& e) {
          r.set("ok", false);
          r.set("error", e.what());
        }
        r.set("ms", t.ms());
        results[i] = r;
      }
    });
  for (auto& t : workers) t.join();
  CommandOutput o;
  o.result = Json::object();
  Json arr = Json::array();
  int ok = 0;
  for (auto& r : results) { if (r.boolean("ok", false)) ok++; arr.push(r); }
  o.result.set("ok", ok == (int)n);
  o.result.set("succeeded", ok);
  o.result.set("failed", (int)n - ok);
  o.result.set("results", arr);
  o.result.set("total_ms", total.ms());
  o.images = images;
  return o;
}

}  // namespace

EvaluatedProject evaluate_project_maps(const std::string& project, const Json* doc_override, int resolution, const std::string& mode) {
  EvaluatedProject ev;
  Project p = load_project(project);
  if (doc_override) p.doc = *doc_override;
  ev.doc = p.doc;
  Json bstats;
  Timer tb;
  ev.bk = get_bake(p, resolution, false, &bstats);
  ev.bake_ms = tb.ms();
  EvalOptions opt;
  if (mode.rfind("mask:", 0) == 0) opt.record_masks.insert(mode.substr(5));
  Timer te;
  auto results = evaluate_all(p, *ev.bk, opt, nullptr, &ev.warnings);
  ev.maps.resize(ev.bk->sets.size());
  for (size_t i = 0; i < results.size(); i++) ev.maps[i] = make_maps(ev.bk->sets[i], results[i], extra_maps_for_mode(mode));
  ev.eval_ms = te.ms();
  return ev;
}

std::vector<std::string> command_names() {
  return {"inspect", "new", "get", "edit", "validate", "bake", "render", "variants", "export", "library", "batch", "blender"};
}

CommandOutput run_command(const std::string& name, const Json& args_in) {
  Json args = args_in.is_object() ? args_in : Json::object();
  if (name == "inspect") return cmd_inspect(args);
  if (name == "new") return cmd_new(args);
  if (name == "get") return cmd_get(args);
  if (name == "edit") return cmd_edit(args);
  if (name == "validate") return cmd_validate(args);
  if (name == "bake") return cmd_bake(args);
  if (name == "render") return cmd_render(args);
  if (name == "variants") return cmd_variants(args);
  if (name == "export") return cmd_export(args);
  if (name == "library") return cmd_library(args);
  if (name == "batch") return cmd_batch(args);
  if (name == "blender") return cmd_blender(args);
  std::string dym = did_you_mean(name, command_names());
  fail("unknown command '%s'%s", name.c_str(), dym.empty() ? "" : (" (did you mean '" + dym + "'?)").c_str());
}

}  // namespace pt
