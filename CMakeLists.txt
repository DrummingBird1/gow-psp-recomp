cmake_minimum_required(VERSION 3.20)
project(gow_recomp C)

# Generated sources come from recomp/recomp.py --out build/gen/<GAME>
set(GAME "UCUS98653" CACHE STRING "Game ID whose generated sources are built")
set(GEN_DIR "${CMAKE_SOURCE_DIR}/build/gen/${GAME}")
file(GLOB GEN_SOURCES CONFIGURE_DEPENDS "${GEN_DIR}/*.c")
if(NOT GEN_SOURCES)
  message(FATAL_ERROR "No generated sources in ${GEN_DIR}; run recomp/recomp.py first")
endif()

add_executable(gow_rt
  runtime/rt_core.c
  runtime/rt_hle.c
  runtime/rt_main.c
  ${GEN_SOURCES})
set_target_properties(gow_rt PROPERTIES OUTPUT_NAME "gow_rt_${GAME}" C_STANDARD 11)
target_include_directories(gow_rt PRIVATE runtime "${GEN_DIR}")

if(MSVC)
  # /bigobj: generated files are large; /fp:precise keeps IEEE single-precision semantics
  target_compile_options(gow_rt PRIVATE /W3 /bigobj /fp:precise /wd4102 /wd4100 /MP)
  target_compile_definitions(gow_rt PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(gow_rt PRIVATE -O2 -fno-strict-aliasing -ffp-contract=off -Wno-unused-label)
  target_link_libraries(gow_rt PRIVATE m)
endif()
