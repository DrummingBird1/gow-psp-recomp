cmake_minimum_required(VERSION 3.20)
project(gow_recomp C)

# Generated sources come from recomp/recomp.py --out build/gen/<GAME>
set(GAME "UCUS98653" CACHE STRING "Game ID whose generated sources are built")
set(GEN_DIR "${CMAKE_SOURCE_DIR}/build/gen/${GAME}")
file(GLOB GEN_SOURCES CONFIGURE_DEPENDS "${GEN_DIR}/*.c")
if(NOT GEN_SOURCES)
  message(FATAL_ERROR "No generated sources in ${GEN_DIR}; run recomp/recomp.py first")
endif()

# libchdr (BSD-3-Clause) reads the user's CHD disc images; pinned to a reviewed commit
include(FetchContent)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(CHDR_WANT_TESTS OFF CACHE BOOL "" FORCE)
set(INSTALL_STATIC_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(libchdr
  GIT_REPOSITORY https://github.com/rtissera/libchdr.git
  GIT_TAG 47d794151b2b64f6aba60304e6480f4f5a2bc24b)
FetchContent_MakeAvailable(libchdr)

add_executable(gow_rt
  runtime/rt_core.c
  runtime/rt_fiber.c
  runtime/rt_hle.c
  runtime/rt_io.c
  runtime/rt_main.c
  runtime/rt_thread.c
  runtime/rt_vfpu.c
  runtime/rt_selftest.c
  ${GEN_SOURCES})
set_target_properties(gow_rt PROPERTIES OUTPUT_NAME "gow_rt_${GAME}" C_STANDARD 11)
target_include_directories(gow_rt PRIVATE runtime "${GEN_DIR}")
target_link_libraries(gow_rt PRIVATE chdr-static)

if(MSVC)
  # /bigobj: generated files are large; /fp:precise keeps IEEE single-precision semantics
  target_compile_options(gow_rt PRIVATE /W3 /bigobj /fp:precise /wd4102 /wd4100 /MP)
  target_compile_definitions(gow_rt PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(gow_rt PRIVATE -O2 -fno-strict-aliasing -ffp-contract=off -Wno-unused-label)
  target_link_libraries(gow_rt PRIVATE m)
endif()
