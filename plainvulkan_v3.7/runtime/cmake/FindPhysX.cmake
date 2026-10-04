# FindPhysX.cmake -- locates the PhysX SDK and defines the imported target
# `PhysX::PhysX`.
#
# Search order, most-specific first:
#   1. -DPHYSX_ROOT=<dir> on the CMake command line
#   2. the PHYSX_ROOT / PHYSX_DIR environment variables
#   3. vcpkg / system package config (find_package(physx CONFIG))
#   4. FetchContent from NVIDIA's public GitHub repo, if PV_FETCH_PHYSX=ON
#
# Sets PHYSX_FOUND. The caller (runtime/CMakeLists.txt) treats a miss as
# non-fatal and falls back to the builtin solver, so a first-time clone
# still builds and runs.

include_guard(GLOBAL)

set(PHYSX_FOUND FALSE)

# --- 1 & 2: explicit root ------------------------------------------------
set(_physx_hints "")
if(DEFINED PHYSX_ROOT)
  list(APPEND _physx_hints "${PHYSX_ROOT}")
endif()
if(DEFINED ENV{PHYSX_ROOT})
  list(APPEND _physx_hints "$ENV{PHYSX_ROOT}")
endif()
if(DEFINED ENV{PHYSX_DIR})
  list(APPEND _physx_hints "$ENV{PHYSX_DIR}")
endif()

find_path(PHYSX_INCLUDE_DIR
  NAMES PxPhysicsAPI.h
  HINTS ${_physx_hints}
  PATH_SUFFIXES include physx/include physx/source/physxapi/include
)

# PhysX ships a different library layout per platform and per configuration.
# On Windows the static libs carry a platform suffix (_64) and live under a
# bin/<platform>/<config> tree; on Linux they're plain .a files under bin/.
if(WIN32)
  set(_physx_lib_suffix "_64")
  set(_physx_path_suffixes
      bin/win.x86_64.vc143.mt/release
      bin/win.x86_64.vc142.mt/release
      bin/win.x86_64.vc141.mt/release
      lib)
else()
  set(_physx_lib_suffix "")
  set(_physx_path_suffixes
      bin/linux.x86_64/release
      bin/linux.clang/release
      lib)
endif()

# Link order matters for the static libraries: PhysX's own libs have
# circular-ish dependencies, and this is the order NVIDIA's own samples use.
set(_physx_components
    PhysXExtensions
    PhysX
    PhysXPvdSDK
    PhysXCooking
    PhysXCommon
    PhysXFoundation)

set(PHYSX_LIBRARIES "")
set(_physx_all_found TRUE)
foreach(_comp ${_physx_components})
  find_library(PHYSX_${_comp}_LIBRARY
    NAMES ${_comp}${_physx_lib_suffix} ${_comp} ${_comp}_static${_physx_lib_suffix}
    HINTS ${_physx_hints}
    PATH_SUFFIXES ${_physx_path_suffixes}
  )
  if(PHYSX_${_comp}_LIBRARY)
    list(APPEND PHYSX_LIBRARIES "${PHYSX_${_comp}_LIBRARY}")
  else()
    set(_physx_all_found FALSE)
  endif()
endforeach()

if(PHYSX_INCLUDE_DIR AND _physx_all_found)
  set(PHYSX_FOUND TRUE)
endif()

# --- 3: vcpkg / system CONFIG package ------------------------------------
if(NOT PHYSX_FOUND)
  find_package(physx CONFIG QUIET)
  if(physx_FOUND)
    set(PHYSX_FOUND TRUE)
    set(PHYSX_VIA_CONFIG TRUE)
  endif()
endif()

# --- 4: fetch from source (opt-in) ---------------------------------------
# Off by default: PhysX is a large repository and a long build, and silently
# starting one on a developer's first `pv build` would be a hostile surprise.
if(NOT PHYSX_FOUND AND PV_FETCH_PHYSX)
  message(STATUS "PhysX not found locally -- fetching from NVIDIA-Omniverse/PhysX (this takes a while)")
  include(FetchContent)
  set(PHYSX_ROOT_DIR "" CACHE PATH "" FORCE)
  FetchContent_Declare(physx
    GIT_REPOSITORY https://github.com/NVIDIA-Omniverse/PhysX.git
    GIT_TAG 106.5-physx-5.5.1
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR physx/compiler/public
  )
  FetchContent_MakeAvailable(physx)
  if(TARGET PhysX)
    set(PHYSX_FOUND TRUE)
    set(PHYSX_VIA_FETCH TRUE)
  endif()
endif()

# --- imported target -----------------------------------------------------
if(PHYSX_FOUND AND NOT TARGET PhysX::PhysX)
  add_library(PhysX::PhysX INTERFACE IMPORTED)
  if(PHYSX_VIA_CONFIG)
    target_link_libraries(PhysX::PhysX INTERFACE
      physx::PhysXExtensions physx::PhysX physx::PhysXPvdSDK physx::PhysXCommon physx::PhysXFoundation)
  elseif(PHYSX_VIA_FETCH)
    target_link_libraries(PhysX::PhysX INTERFACE
      PhysXExtensions PhysX PhysXPvdSDK PhysXCooking PhysXCommon PhysXFoundation)
  else()
    target_include_directories(PhysX::PhysX INTERFACE "${PHYSX_INCLUDE_DIR}")
    target_link_libraries(PhysX::PhysX INTERFACE ${PHYSX_LIBRARIES})
  endif()

  if(UNIX)
    # PhysX's task manager uses pthreads directly.
    find_package(Threads REQUIRED)
    target_link_libraries(PhysX::PhysX INTERFACE Threads::Threads)
  endif()

  # PhysX headers are noisy under the warning levels this project builds
  # with; SYSTEM include semantics keep third-party warnings out of our log.
  set_target_properties(PhysX::PhysX PROPERTIES
    INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${PHYSX_INCLUDE_DIR}")
endif()

if(PHYSX_FOUND)
  message(STATUS "PhysX: found (${PHYSX_INCLUDE_DIR})")
else()
  message(STATUS "PhysX: not found -- the runtime will use its builtin fallback solver. "
                 "Set -DPHYSX_ROOT=<sdk> or -DPV_FETCH_PHYSX=ON to enable it.")
endif()
