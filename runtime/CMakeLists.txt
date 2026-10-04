cmake_minimum_required(VERSION 3.16)
project(pv_runtime CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(PV_WITH_PHYSX "Use NVIDIA PhysX for physics (falls back to the builtin solver if not found)" ON)
option(PV_FETCH_PHYSX "Download and build PhysX from source if it isn't installed" OFF)

# --- MSVC / Windows ------------------------------------------------------
if(MSVC)
  # /utf-8      -- source and execution charset. Without it MSVC reads this
  #                codebase's UTF-8 string literals in the system code page.
  # /permissive- -- standards conformance; catches code that only compiles
  #                because of MSVC's older non-conforming lookup rules.
  # /Zc:__cplusplus -- otherwise __cplusplus reports 199711L even in C++17
  #                mode, which breaks feature detection in vendored headers.
  # /MP         -- parallel compilation, since MSVC doesn't do it by default.
  add_compile_options(/utf-8 /permissive- /Zc:__cplusplus /MP)
  # miniaudio.h and stb are large single-header libraries that legitimately
  # trip these; silencing them keeps our own warnings visible.
  add_compile_options(/wd4996 /wd4267 /wd4244)
  # PhysX's static libraries are built against the dynamic CRT (the "md"
  # flavours) or static ("mt"); mismatching the runtime library here is the
  # classic cause of duplicate-symbol and heap-corruption errors on Windows.
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL" CACHE STRING "")
endif()

find_package(Vulkan REQUIRED)

# --- GLFW ----------------------------------------------------------------
find_package(glfw3 3.3 QUIET)
if(NOT glfw3_FOUND)
  message(STATUS "System GLFW not found -- fetching source via CMake FetchContent")
  include(FetchContent)
  set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(glfw GIT_REPOSITORY https://github.com/glfw/glfw.git GIT_TAG 3.4)
  FetchContent_MakeAvailable(glfw)
endif()

# --- PhysX ---------------------------------------------------------------
set(PV_PHYSX_ENABLED FALSE)
if(PV_WITH_PHYSX)
  include(FindPhysX)
  if(PHYSX_FOUND)
    set(PV_PHYSX_ENABLED TRUE)
  endif()
endif()

# --- Shader compilation --------------------------------------------------
# glslangValidator ships with the Vulkan SDK; on Linux it's also in
# glslang-tools. glslc (from shaderc) is accepted as an alternative because
# it's what the Android and some Windows SDK layouts provide.
find_program(GLSLANG_VALIDATOR NAMES glslangValidator glslangValidator.exe)
find_program(GLSLC NAMES glslc glslc.exe)
if(NOT GLSLANG_VALIDATOR AND NOT GLSLC)
  message(FATAL_ERROR
    "No GLSL compiler found. Install the Vulkan SDK (which provides glslangValidator "
    "and glslc) or your distribution's glslang-tools package.")
endif()

file(GLOB PV_SHADER_SOURCES
  "${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.vert"
  "${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.frag")
set(PV_SHADER_OUTPUT_DIR "${CMAKE_BINARY_DIR}/shaders_out")
file(MAKE_DIRECTORY ${PV_SHADER_OUTPUT_DIR})

set(PV_COMPILED_SHADERS "")
foreach(SHADER ${PV_SHADER_SOURCES})
  get_filename_component(SHADER_NAME ${SHADER} NAME)
  set(SHADER_OUT "${PV_SHADER_OUTPUT_DIR}/${SHADER_NAME}.spv")
  if(GLSLANG_VALIDATOR)
    set(SHADER_CMD ${GLSLANG_VALIDATOR} -V ${SHADER} -o ${SHADER_OUT})
  else()
    set(SHADER_CMD ${GLSLC} ${SHADER} -o ${SHADER_OUT})
  endif()
  add_custom_command(
    OUTPUT ${SHADER_OUT}
    COMMAND ${SHADER_CMD}
    DEPENDS ${SHADER}
    COMMENT "Compiling shader ${SHADER_NAME}")
  list(APPEND PV_COMPILED_SHADERS ${SHADER_OUT})
endforeach()
add_custom_target(pv_shaders ALL DEPENDS ${PV_COMPILED_SHADERS})
set(PV_SHADER_OUTPUT_DIR ${PV_SHADER_OUTPUT_DIR} PARENT_SCOPE)

# Recast, Detour, and RVO2 from runtime/third_party/Ai (official GitHub trees).
# Only the libraries themselves -- not RecastDemo, tests, or RVO examples.
file(GLOB PV_RECAST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/third_party/Ai/recastnavigation/Recast/Source/*.cpp")
file(GLOB PV_DETOUR_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/third_party/Ai/recastnavigation/Detour/Source/*.cpp")
file(GLOB PV_RVO2_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/third_party/Ai/RVO2/src/*.cc")
set(PV_AI_VENDOR_SOURCES ${PV_RECAST_SOURCES} ${PV_DETOUR_SOURCES} ${PV_RVO2_SOURCES})

# --- pv_runtime static library -------------------------------------------
add_library(pv_runtime STATIC
  src/pv_value.cpp
  src/pv_math.cpp
  src/pv_json.cpp
  src/vk_core.cpp
  src/window.cpp
  src/frame.cpp
  src/camera.cpp
  src/draw2d.cpp
  src/draw3d.cpp
  src/font_data.cpp
  src/mesh.cpp
  src/texture.cpp
  src/shader.cpp
  src/lighting.cpp
  src/material.cpp
  src/audio.cpp
  src/input.cpp
  src/physics.cpp
  src/physics_builtin.cpp
  src/physics_physx.cpp
  src/filesystem.cpp
  src/ui.cpp
  src/debug.cpp
  src/animation.cpp
  src/anim_sample.cpp
  src/animation_gltf.cpp
  src/particles.cpp
  src/scene.cpp
  src/scene_io.cpp
  src/scene_commands.cpp
  src/network.cpp
  src/network_scene.cpp
  src/crypto.cpp
  src/post.cpp
  third_party/monocypher.c
  # Terrain and AI. terrain.cpp and ai.cpp deliberately contain no Vulkan
  # (see the headers) so runtime_tests can link them directly; only
  # terrain_render.cpp touches the device.
  src/terrain.cpp
  src/terrain_render.cpp
  src/ai.cpp
  src/ai_navmesh.cpp
  ${PV_AI_VENDOR_SOURCES}
)
add_dependencies(pv_runtime pv_shaders)

target_include_directories(pv_runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(pv_runtime PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/Ai/RVO2/src
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/Ai/recastnavigation/Recast/Include
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/Ai/recastnavigation/Detour/Include)
target_compile_definitions(pv_runtime PRIVATE RVO_STATIC_DEFINE)
# Monocypher is C99; keep it as C so MSVC does not compile it as C++.
set_source_files_properties(third_party/monocypher.c PROPERTIES LANGUAGE C)

target_link_libraries(pv_runtime PUBLIC Vulkan::Vulkan glfw)
find_package(Threads REQUIRED)
target_link_libraries(pv_runtime PUBLIC Threads::Threads)

if(PV_PHYSX_ENABLED)
  target_link_libraries(pv_runtime PUBLIC PhysX::PhysX)
  target_compile_definitions(pv_runtime PRIVATE PV_WITH_PHYSX=1)
  message(STATUS "Physics backend: PhysX")
else()
  message(STATUS "Physics backend: builtin fallback (see runtime/README.md)")
endif()

if(UNIX AND NOT APPLE)
  target_link_libraries(pv_runtime PUBLIC dl m)
endif()

if(WIN32)
  # winmm: miniaudio's WinMM backend. The rest are pulled in by GLFW and by
  # PhysX's use of the Windows thread/timer APIs.
  # ws2_32: the Winsock 2 implementation behind runtime/src/network.cpp.
  target_link_libraries(pv_runtime PUBLIC winmm ws2_32)
  target_compile_definitions(pv_runtime PUBLIC
    NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)
endif()
