# Windows can't overwrite a running .exe (e.g. build/patina.exe while Claude Code runs it as the MCP server
# from .mcp.json), but it can rename it. Move it aside before linking; leftovers are removed once unlocked.
file(GLOB _old "${EXE}.old-*")
foreach(_f ${_old})
  execute_process(COMMAND ${CMAKE_COMMAND} -E rm -f "${_f}" ERROR_QUIET RESULT_VARIABLE _ignored)
endforeach()
if(EXISTS "${EXE}")
  string(RANDOM LENGTH 8 _id)
  file(RENAME "${EXE}" "${EXE}.old-${_id}")
endif()
