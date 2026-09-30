// Built-in content: smart materials (parameterized layer templates) and export presets.
#pragma once
#include "json.h"

namespace pt {

const Json& builtin_library();
// Smart materials from the built-in library plus <project_dir>/library/*.json and $PATINA_LIBRARY dirs.
Json smart_materials(const std::string& project_dir);
// Expand a {"type":"smart","material":...,"params":{...}} layer into a folder layer. Inner layer ids
// are prefixed with the outer id ("rust/paint") so several instances never collide.
Json expand_smart_material(const Json& layer, const std::string& outer_id, const std::string& project_dir, std::vector<std::string>& warnings);
Json smart_material_catalog(const std::string& project_dir);
const Json* find_export_preset(const std::string& name);
Json export_preset_catalog();

}  // namespace pt
