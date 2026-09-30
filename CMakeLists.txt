cmake_minimum_required(VERSION 3.20)
project(patina VERSION 0.3.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(PATINA_VIEWER "Build the native viewer window (patina view)" ON)

if(MSVC)
  add_compile_options(/W3 /MP /fp:fast /D_CRT_SECURE_NO_WARNINGS /DNOMINMAX /utf-8)
  set(CMAKE_CXX_FLAGS_RELEASE "/O2 /Ob2 /DNDEBUG")
else()
  add_compile_options(-Wall -Wno-unused-function -Wno-missing-field-initializers -fno-math-errno)
  set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
  set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG")
endif()

# ---- third-party single-header implementations (compiled once, rarely rebuilt)
add_library(patina_thirdparty STATIC src/thirdparty_impl.c third_party/mikktspace/mikktspace.c)
target_include_directories(patina_thirdparty PUBLIC third_party third_party/mikktspace)

# ---- engine core (everything except the CLI entry point and the viewer)
add_library(patina_core STATIC
  src/util.cpp
  src/parallel.cpp
  src/json.cpp
  src/mesh.cpp
  src/bvh.cpp
  src/bake.cpp
  src/bake_normal.cpp
  src/noise.cpp
  src/image.cpp
  src/eval.cpp
  src/library.cpp
  src/export.cpp
  src/render.cpp
  src/envmap.cpp
  src/project.cpp
  src/commands.cpp
  src/mcp.cpp
  src/update.cpp
)
target_include_directories(patina_core PUBLIC src third_party)

# Embed files as byte arrays (cmake/embed.c, built first): docs/AGENT_GUIDE.md for `library topic=guide`,
# `patina guide` and the installed skill; the Blender bridge so an installed binary can run `blender`
# without the repo; the default studio HDRI (CC0, see third_party/hdri/README.txt) for renders.
add_executable(patina_embed cmake/embed.c)
function(patina_embed file name)
  set(out ${CMAKE_BINARY_DIR}/generated/${name}.inc)
  add_custom_command(OUTPUT ${out}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/generated
    COMMAND patina_embed ${file} ${out}
    DEPENDS patina_embed ${file} VERBATIM)
  target_sources(patina_core PRIVATE ${out})
  set_source_files_properties(${out} PROPERTIES HEADER_FILE_ONLY TRUE)
endfunction()
patina_embed(${CMAKE_SOURCE_DIR}/docs/AGENT_GUIDE.md patina_guide)
patina_embed(${CMAKE_SOURCE_DIR}/tools/blender/patina_blender.py patina_blender)
patina_embed(${CMAKE_SOURCE_DIR}/third_party/hdri/studio_small_09_1k.hdr patina_studio_hdri)
target_include_directories(patina_core PRIVATE ${CMAKE_BINARY_DIR}/generated)
target_compile_definitions(patina_core PUBLIC PATINA_VERSION="${PROJECT_VERSION}")
target_link_libraries(patina_core PUBLIC patina_thirdparty)
find_package(Threads REQUIRED)
target_link_libraries(patina_core PUBLIC Threads::Threads)
if(WIN32)
  target_link_libraries(patina_core PUBLIC advapi32 user32)  # install: user PATH in the registry
endif()

add_executable(patina src/main.cpp)
target_link_libraries(patina PRIVATE patina_core)
if(WIN32)
  add_custom_command(TARGET patina PRE_LINK COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:patina> -P ${CMAKE_SOURCE_DIR}/cmake/move_aside.cmake)
endif()

if(PATINA_VIEWER)
  add_library(patina_imgui STATIC
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
  )
  target_include_directories(patina_imgui PUBLIC third_party/imgui)

  if(APPLE)
    set(SOKOL_BACKEND SOKOL_METAL)
    set(SOKOL_IMPL_SRC src/sokol_impl.m)
    set_source_files_properties(src/sokol_impl.m PROPERTIES COMPILE_FLAGS "-fobjc-arc")
  elseif(WIN32)
    set(SOKOL_BACKEND SOKOL_D3D11)
    set(SOKOL_IMPL_SRC src/sokol_impl.c)
  else()
    set(SOKOL_BACKEND SOKOL_GLCORE)
    set(SOKOL_IMPL_SRC src/sokol_impl.c)
  endif()

  target_sources(patina PRIVATE src/viewer.cpp ${SOKOL_IMPL_SRC})
  target_compile_definitions(patina PRIVATE PATINA_HAS_VIEWER=1 SOKOL_NO_ENTRY ${SOKOL_BACKEND})
  target_link_libraries(patina PRIVATE patina_imgui)
  if(APPLE)
    target_link_libraries(patina PRIVATE "-framework Cocoa" "-framework QuartzCore" "-framework Metal" "-framework MetalKit")
  elseif(WIN32)
    target_link_libraries(patina PRIVATE d3d11 dxgi)
    # keep a console subsystem so the same binary works as CLI + MCP server
  else()
    target_link_libraries(patina PRIVATE X11 Xi Xcursor GL dl m)
  endif()
endif()
