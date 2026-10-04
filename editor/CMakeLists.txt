cmake_minimum_required(VERSION 3.16)
project(pvedit CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The editor is the one part of PlainVulkan with a third-party UI
# dependency. It's fetched rather than vendored because it's only needed to
# build the editor -- a game binary links pv_runtime and nothing here.
# Vulkan::Vulkan and glfw are imported targets created by find_package() in
# runtime/CMakeLists.txt. Imported targets are *directory-scoped*, so they
# are NOT visible here even though the runtime is added as a sibling
# subdirectory first -- configuring the editor failed outright with
# 'Target "imgui" links to Vulkan::Vulkan but the target was not found'.
# Re-finding them in this directory is the supported fix.
find_package(Vulkan REQUIRED)
# glfw may instead be a real (non-imported, therefore globally visible)
# target that the runtime pulled in via FetchContent when no system GLFW was
# installed; only go looking for a package if that didn't happen.
if(NOT TARGET glfw)
  find_package(glfw3 3.3 REQUIRED)
endif()

include(FetchContent)
# NOTE the "-docking" suffix. Docking (ImGuiConfigFlags_DockingEnable,
# DockSpaceOverViewport, ImGuiDockNodeFlags_*) is NOT gated behind a
# preprocessor define -- it only exists on ImGui's separate `docking`
# branch. Pinning the plain v1.91.5 tag built an ImGui without any of those
# symbols, so pvedit.cpp failed to compile on the three lines that set up
# the dock space. v1.91.5-docking is the same release, from that branch.
FetchContent_Declare(imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG v1.91.5-docking
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(imgui)

# ImGui ships no CMakeLists of its own -- it's meant to be compiled into the
# consuming target, which is what this does. Only the two backends this
# editor uses (GLFW for input/windowing, Vulkan for rendering) are built.
add_library(imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/imgui_demo.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui PUBLIC Vulkan::Vulkan glfw)

# Enables ImVec2/ImVec4 arithmetic operators (v1 + v2, v * f, ...), which
# the gizmo and viewport code uses. Unrelated to docking -- that comes from
# the branch pinned above, not from a define.
target_compile_definitions(imgui PUBLIC IMGUI_DEFINE_MATH_OPERATORS)

add_executable(pvedit src/pvedit.cpp src/gizmo.cpp)
target_link_libraries(pvedit PRIVATE pv_runtime imgui)

set_target_properties(pvedit PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
# Visual Studio / Xcode append a config subdir (bin/RelWithDebInfo/) unless
# every RUNTIME_OUTPUT_DIRECTORY_<CONFIG> is set. pv edit looks in bin/.
foreach(CFG ${CMAKE_CONFIGURATION_TYPES})
  string(TOUPPER ${CFG} CFG_UPPER)
  set_target_properties(pvedit PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_${CFG_UPPER} "${CMAKE_BINARY_DIR}/bin")
endforeach()

# Copy SPIR-V next to the actual executable (generator expressions follow
# the real output dir even if a multi-config generator is used).
add_custom_command(TARGET pvedit POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:pvedit>/shaders"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${PV_SHADER_OUTPUT_DIR}" "$<TARGET_FILE_DIR:pvedit>/shaders"
  COMMENT "Staging compiled shaders next to pvedit")

if(WIN32)
  # Console subsystem for the editor specifically: an editor's log output is
  # something you want to see, unlike a shipped game's.
  set_target_properties(pvedit PROPERTIES WIN32_EXECUTABLE FALSE)
endif()
