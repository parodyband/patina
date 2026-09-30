// Operations shared by the CLI and the MCP server. Every command takes and returns JSON.
#pragma once
#include <memory>

#include "eval.h"
#include "json.h"

namespace pt {

struct CommandOutput {
  Json result;
  std::vector<std::string> images;  // encoded PNG bytes (MCP returns these as image content)
};

// Dispatches a command by name ("inspect", "new", "get", "edit", "validate", "bake", "render",
// "variants", "export", "library", "batch", "blender"). Throws pt::Error on failure.
CommandOutput run_command(const std::string& name, const Json& args);
std::vector<std::string> command_names();
const char* agent_guide();  // docs/AGENT_GUIDE.md, embedded at build time
const char* mcp_instructions();  // the MCP server's instructions (also used in the installed skill)
std::string shell_quote(const std::string& s);

// install / update (update.cpp)
const char* patina_version();
std::string patina_home();              // ~/.patina, or $PATINA_HOME
std::string installed_bridge_script();  // <home>/tools/blender/patina_blender.py, written if missing
CommandOutput cmd_install(const Json& args);
CommandOutput cmd_update(const Json& args);
void start_update_check();   // background check for a newer release, at most once a day
std::string update_notice();  // "Patina X is available ..." from the last check, or ""

// Load + bake (cached) + evaluate a project into padded maps (used by the viewer).
struct EvaluatedProject {
  std::shared_ptr<Baked> bk;
  std::vector<SetMaps> maps;
  Json doc;
  Json warnings;
  double bake_ms = 0, eval_ms = 0;
};
EvaluatedProject evaluate_project_maps(const std::string& project, const Json* doc_override, int resolution, const std::string& mode);

}  // namespace pt
