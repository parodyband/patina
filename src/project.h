// Project documents: a JSON file describing the mesh, bake settings and per-texture-set layer stacks.
#pragma once
#include "bake.h"
#include "json.h"
#include "mesh.h"

namespace pt {

struct Project {
  std::string path;  // absolute path of the project file
  std::string dir;   // directory used to resolve relative paths
  Json doc;

  std::string resolve(const std::string& rel) const { return rel.empty() ? rel : path_join(dir, rel); }
  std::string mesh_path() const;
  int resolution() const;
  int set_resolution(const std::string& set) const;
  BakeSettings bake_settings() const;
  float height_depth() const;  // fraction of the mesh's largest dimension for height = 1
  const Json& layers(const std::string& set) const;
  std::string cache_dir() const;
  std::string name() const;
};

Project load_project(const std::string& path);
Project project_from_doc(const std::string& path, Json doc);
void save_project(const Project& p);
Json new_project_doc(const std::string& mesh_rel, const Mesh& m, int resolution);

// Layer bookkeeping. Ids are unique across the whole project.
void assign_layer_ids(Json& doc);
struct LayerRef {
  Json* layer = nullptr;
  Json* container = nullptr;  // the array holding the layer
  size_t index = 0;
  std::string set;
};
LayerRef find_layer(Json& doc, const std::string& id);
std::vector<std::string> all_layer_ids(const Json& doc);

// Apply a list of edit operations (add/update/remove/move/replace/set/duplicate). Throws on error.
Json apply_edit_ops(Json& doc, const Json& ops);

// Structural validation (unknown keys, bad types, unknown texture sets). Evaluation adds more.
Json validate_project(const Project& p, const Mesh* m);

// Compact outline of the layer stacks for agents (ids, types, names, what they touch).
Json project_outline(const Project& p);

}  // namespace pt
