cmake_minimum_required(VERSION 3.20)
project(gow_recomp C CXX)

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

# SDL3 (zlib license): the game window, keyboard and gamepad input; pinned to release 3.4.16
option(GOW_WINDOW "Build the SDL3 window and input (gow_rt --window)" ON)
if(GOW_WINDOW)
  set(SDL_SHARED OFF CACHE BOOL "" FORCE)
  set(SDL_STATIC ON CACHE BOOL "" FORCE)
  set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
  set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG fa2c02bb6e21974a89ea9824bc53c9932abe5f9c)
  FetchContent_MakeAvailable(SDL3)
endif()

# ATRAC3/ATRAC3+ decoder for sceAtrac3plus: PPSSPP's ext/at3_standalone (FFmpeg's decoders
# extracted to build alone, LGPL-2.1+), downloaded from tag v1.20.4 and checked by SHA-256.
# The sources stay unmodified; runtime/at3shim stands in for the few PPSSPP headers they include.
set(AT3_DIR "${CMAKE_BINARY_DIR}/_deps/at3_standalone")
set(AT3_URL "https://raw.githubusercontent.com/hrydgard/ppsspp/v1.20.4/ext/at3_standalone")
set(AT3_FILES
  at3_decoders.h=ed0bfaf3f6e5e952a6c292ca45b5699e19baf76d0110a1e185a0626d3e5d642f
  atrac.cpp=909bae48dcf366f1fc5f17e6cf49e996076f780e2c1fe147de5fb2b06ea59dd0
  atrac.h=d3af304148aac3c638ca66414f15288ae74719a5e931da8ba927e1814766df19
  atrac3.cpp=cb620e9d80c8093cfef598e4df3ea339a02c1dc644c2f63b769575e4d8bf2465
  atrac3data.h=7f333b5652e24d6234469745b2b2ac7cb6df50343d9c7c9af009b5c6eb3fb8c9
  atrac3plus.cpp=109c0329b47278be638f02426a562d43351366ef464d63253dc73cdd10534422
  atrac3plus.h=555b9a0b5c176a9364b29bfc7761476ef1189c5a7bbba9907d7ae8e71b46325b
  atrac3plus_data.h=d6aaff5c1bb5351f5294fbc288ad1b199534c99621c68c587b1254566631a018
  atrac3plusdec.cpp=fb38cb1b2f0b67aa79e536fcf26371f83529b30622496889fd9bf1846a94a7e9
  atrac3plusdsp.cpp=3c89356b15dd4bab3b3a4897dd213d720a7d3b8348dda06c88bcd55cf8b61294
  compat.cpp=8d379eb3a64f6148761cd18331ab27ba337ddc4531e82e440b9fe402ff39237d
  compat.h=790717da221d524a1f1490b500703b26b399b10173696787bc88545fd126d1e0
  fft.cpp=1f356f7f583806cf1a925aa96db1d9248ce828b4e7340565aedc45cc2f62e3ce
  fft.h=261e06b21bce15c4ace6b5b915ab0205f541fff5987c6d7e1706653cbf298850
  float_dsp.h=a422fd31275acaaca2f5dec122a8d9aa1f8ece507e45f6b1433463a668598903
  get_bits.cpp=52d330db740ceb3f409f0624e5f4f3f7b1dc2bec614a696da315fbdff3fd1e5b
  get_bits.h=a7b7607394aad6af221c01c2b37ad1d12b64d15bf6554cdcbaf8510dcb4dc747
  intreadwrite.h=554b87abca170dcf3bf0d56d12112aefd7be61f1218dc6190f59bf5dfef00a72
  mem.cpp=5899087c84397db946fa91d737a70ba6589449ea74115b0e438f48bebd030cad
  mem.h=3b4c78a4ba926548300f67d2a860417d5c2ef6a4d8d13c92c320205f07223602)
set(AT3_SOURCES)
foreach(entry IN LISTS AT3_FILES)
  string(REPLACE "=" ";" parts "${entry}")
  list(GET parts 0 name)
  list(GET parts 1 hash)
  set(path "${AT3_DIR}/${name}")
  set(have "")
  if(EXISTS "${path}")
    file(SHA256 "${path}" have)
  endif()
  if(NOT have STREQUAL hash)
    file(DOWNLOAD "${AT3_URL}/${name}" "${path}" EXPECTED_HASH SHA256=${hash} TLS_VERIFY ON STATUS st)
    list(GET st 0 code)
    if(NOT code EQUAL 0)
      message(FATAL_ERROR "Downloading at3_standalone/${name} failed: ${st}")
    endif()
  endif()
  if(name MATCHES "\\.cpp$")
    list(APPEND AT3_SOURCES "${path}")
  endif()
endforeach()
add_library(at3_standalone STATIC ${AT3_SOURCES} runtime/rt_at3dec.cpp)
target_include_directories(at3_standalone PRIVATE "${AT3_DIR}" runtime/at3shim runtime)
set_target_properties(at3_standalone PROPERTIES CXX_STANDARD 17)
if(MSVC)
  target_compile_options(at3_standalone PRIVATE /W0 /fp:precise)
  target_compile_definitions(at3_standalone PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(at3_standalone PRIVATE -O2 -w)
endif()

add_executable(gow_rt
  runtime/rt_atrac.c
  runtime/rt_audio.c
  runtime/rt_core.c
  runtime/rt_coro.c
  runtime/rt_fiber.c
  runtime/rt_frames.c
  runtime/rt_ge.c
  runtime/rt_gesoft.c
  runtime/rt_gpu.c
  runtime/rt_hle.c
  runtime/rt_host.c
  runtime/rt_io.c
  runtime/rt_main.c
  runtime/rt_mpeg.c
  runtime/rt_prof.c
  runtime/rt_sas.c
  runtime/rt_thread.c
  runtime/rt_vfpu.c
  runtime/rt_selftest.c
  ${GEN_SOURCES})
set_target_properties(gow_rt PROPERTIES OUTPUT_NAME "gow_rt_${GAME}" C_STANDARD 11)
target_include_directories(gow_rt PRIVATE runtime "${GEN_DIR}")
target_link_libraries(gow_rt PRIVATE chdr-static at3_standalone)
if(GOW_WINDOW)
  target_link_libraries(gow_rt PRIVATE SDL3::SDL3-static)
  target_compile_definitions(gow_rt PRIVATE GOW_HAVE_SDL)
endif()

if(MSVC)
  # /bigobj: generated files are large; /fp:precise keeps IEEE single-precision semantics
  target_compile_options(gow_rt PRIVATE /W3 /bigobj /fp:precise /wd4102 /wd4100 /MP)
  target_compile_definitions(gow_rt PRIVATE _CRT_SECURE_NO_WARNINGS)
  # A PDB for Release too: the sampling profiler (--profile) resolves function names from it
  target_compile_options(gow_rt PRIVATE $<$<CONFIG:Release>:/Zi>)
  target_link_options(gow_rt PRIVATE $<$<CONFIG:Release>:/DEBUG /OPT:REF /OPT:ICF>)
  target_link_libraries(gow_rt PRIVATE dbghelp winmm)
  # the renderer's per-pixel functions are hot; stack cookies cost several percent there
  set_source_files_properties(runtime/rt_gesoft.c PROPERTIES COMPILE_OPTIONS /GS-)
else()
  target_compile_options(gow_rt PRIVATE -O2 -fno-strict-aliasing -ffp-contract=off -Wno-unused-label)
  target_link_libraries(gow_rt PRIVATE m)
endif()
