cmake_minimum_required(VERSION 3.20)
project(gow_recomp C CXX)

# GOW_TOOLS_ONLY builds just gow_disc (disc ID, EBOOT decryption), which the launcher needs before
# the game is recompiled; it needs no generated sources.
option(GOW_TOOLS_ONLY "Build only gow_disc, without generated sources" OFF)

# Generated sources come from recomp/recomp.py --out build/gen/<GAME>
set(GAME "UCUS98653" CACHE STRING "Game ID whose generated sources are built")
set(GEN_DIR "${CMAKE_SOURCE_DIR}/build/gen/${GAME}")
file(GLOB GEN_SOURCES CONFIGURE_DEPENDS "${GEN_DIR}/*.c")
if(NOT GEN_SOURCES AND NOT GOW_TOOLS_ONLY)
  message(FATAL_ERROR "No generated sources in ${GEN_DIR}; run recomp/recomp.py first")
endif()

# Third-party sources are downloaded once into GOW_DEPS_DIR and shared by every build folder (the launcher has three:
# cmake_tools, cmake_full, cmake_gos_full); before, each one cloned and downloaded everything again.
# Each source is a pinned GitHub archive of one commit (the folder name carries it), checked by SHA-256; single
# files by their SHA-256. No Git needed: players' PCs usually have none (tools/check_no_git.py guards this).
set(GOW_DEPS_DIR "${CMAKE_SOURCE_DIR}/build/deps" CACHE PATH "Shared download folder for third-party sources")
include(FetchContent)
macro(gow_fetch_archive name repo commit sha256)
  string(TOUPPER "${name}" _gow_upper)
  set(_gow_src "${GOW_DEPS_DIR}/${name}-${commit}")
  if(EXISTS "${_gow_src}/.gow_complete")
    set(FETCHCONTENT_SOURCE_DIR_${_gow_upper} "${_gow_src}")  # already downloaded: reuse
  endif()
  FetchContent_Declare(${name}
    URL "https://github.com/${repo}/archive/${commit}.tar.gz"
    URL_HASH SHA256=${sha256}
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_DIR "${_gow_src}")
  FetchContent_MakeAvailable(${name})
  file(TOUCH "${_gow_src}/.gow_complete")
endmacro()

# libchdr (BSD-3-Clause) reads the user's CHD disc images; pinned to a reviewed commit
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(CHDR_WANT_TESTS OFF CACHE BOOL "" FORCE)
set(INSTALL_STATIC_LIBS OFF CACHE BOOL "" FORCE)
gow_fetch_archive(libchdr rtissera/libchdr 47d794151b2b64f6aba60304e6480f4f5a2bc24b
  ee1131e748a2c7d7559b7054540ebe49637433901f92071560df52e369f7c7af)  # zlib/zstd/lzma are in its deps/, no submodules

# SDL3 (zlib license): the game window, keyboard and gamepad input; pinned to release 3.4.16
option(GOW_ANDROID "Build libmain.so for the Android SDL shell" OFF)
set(GOW_SDL_SO "" CACHE FILEPATH "libSDL3.so for Android")
option(GOW_WINDOW "Build the SDL3 window and input (gow_rt --window)" ON)
if(GOW_WINDOW AND NOT GOW_TOOLS_ONLY AND NOT GOW_ANDROID)
  set(SDL_SHARED OFF CACHE BOOL "" FORCE)
  set(SDL_STATIC ON CACHE BOOL "" FORCE)
  set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
  set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
  gow_fetch_archive(SDL3 libsdl-org/SDL fa2c02bb6e21974a89ea9824bc53c9932abe5f9c
    d246967187a4f37cc850608dbaf5cff9d1df4d731259903d25539755cdfcc657)
endif()

# ATRAC3/ATRAC3+ decoder for sceAtrac3plus: PPSSPP's ext/at3_standalone (FFmpeg's decoders
# extracted to build alone, LGPL-2.1+), downloaded from tag v1.20.4 and checked by SHA-256.
# The sources stay unmodified; runtime/at3shim stands in for the few PPSSPP headers they include.
set(AT3_DIR "${GOW_DEPS_DIR}/at3_standalone")
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
# Downloads each name=sha256 entry of a list from url/name into dir unless the copy there already
# has that hash; the .c and .cpp files are returned in out_sources.
function(fetch_pinned_files label dir url files out_sources)
  set(sources)
  foreach(entry IN LISTS files)
    string(REPLACE "=" ";" parts "${entry}")
    list(GET parts 0 name)
    list(GET parts 1 hash)
    set(path "${dir}/${name}")
    set(have "")
    if(EXISTS "${path}")
      file(SHA256 "${path}" have)
    endif()
    if(NOT have STREQUAL hash)
      file(DOWNLOAD "${url}/${name}" "${path}" EXPECTED_HASH SHA256=${hash} TLS_VERIFY ON STATUS st)
      list(GET st 0 code)
      if(NOT code EQUAL 0)
        message(FATAL_ERROR "Downloading ${label}/${name} failed: ${st}")
      endif()
    endif()
    if(name MATCHES "\\.(c|cpp)$")
      list(APPEND sources "${path}")
    endif()
  endforeach()
  set(${out_sources} "${sources}" PARENT_SCOPE)
endfunction()

fetch_pinned_files(at3_standalone "${AT3_DIR}" "${AT3_URL}" "${AT3_FILES}" AT3_SOURCES)
add_library(at3_standalone STATIC ${AT3_SOURCES} runtime/rt_at3dec.cpp)
target_include_directories(at3_standalone PRIVATE "${AT3_DIR}" runtime/at3shim runtime)
set_target_properties(at3_standalone PROPERTIES CXX_STANDARD 17)
if(MSVC)
  target_compile_options(at3_standalone PRIVATE /W0 /fp:precise)
  target_compile_definitions(at3_standalone PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(at3_standalone PRIVATE -O2 -w)
endif()

# H.264 decoder for the PSMF movies (sceMpeg): OpenH264's decoder (Cisco, BSD-2-Clause), built
# from source without its assembly. cmake/openh264-v2.6.0.sha256 lists the files of tag v2.6.0
# it needs, each downloaded and checked by SHA-256 (the release archive is 60 MB of test streams).
set(H264_DIR "${GOW_DEPS_DIR}/openh264")
set(H264_URL "https://raw.githubusercontent.com/cisco/openh264/v2.6.0")
file(STRINGS cmake/openh264-v2.6.0.sha256 H264_MANIFEST REGEX "^[0-9a-f]+  ")
set(H264_FILES)
foreach(line IN LISTS H264_MANIFEST)
  string(REGEX MATCH "^([0-9a-f]+)  (.+)$" m "${line}")
  list(APPEND H264_FILES "${CMAKE_MATCH_2}=${CMAKE_MATCH_1}")
endforeach()
fetch_pinned_files(openh264 "${H264_DIR}" "${H264_URL}" "${H264_FILES}" H264_SOURCES)
add_library(openh264_dec STATIC ${H264_SOURCES} runtime/rt_h264dec.cpp)
target_include_directories(openh264_dec PRIVATE "${H264_DIR}/codec/api/wels" "${H264_DIR}/codec/common/inc"
  "${H264_DIR}/codec/decoder/core/inc" "${H264_DIR}/codec/decoder/plus/inc" runtime)
set_target_properties(openh264_dec PROPERTIES CXX_STANDARD 17)
if(MSVC)
  target_compile_options(openh264_dec PRIVATE /W0 /MP)
  target_compile_definitions(openh264_dec PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(openh264_dec PRIVATE -O2 -w)
  target_link_libraries(openh264_dec PRIVATE pthread)
endif()

# PGD decryption (Ghost of Sparta's encrypted data files, sceIoIoctl 0x04100001): PPSSPP's
# ext/libkirk (KIRK engine by Draan et al., GPL-3.0+; amctrl by tpu; AES and SHA-1 public domain;
# bn/ec by Segher Boessenkool, GPL-2.0/3.0), downloaded from tag v1.20.4 and checked by SHA-256.
# It holds the PSP's crypto keys, so it is never part of this repository.
set(KIRK_DIR "${GOW_DEPS_DIR}/libkirk")
set(KIRK_URL "https://raw.githubusercontent.com/hrydgard/ppsspp/v1.20.4/ext/libkirk")
set(KIRK_FILES
  AES.c=3e0321521c1985aceac86d0b5388ab5256a58b94b82092ef48c658756b19d09f
  AES.h=61e784ebd7ddcb6f7876f1933c1e2020f7fc89ab3a1f3bfcb50c29cd35abf179
  SHA1.c=7ef8ee993a7858ff78fdf22efb8c2e06b4864513f6842396b1cf7035f7604024
  SHA1.h=527c0407568beea5b3a07675ec1e25cf477208cd68ba2e45d462fdcb1cd21237
  amctrl.c=2bba661c23c028004ba796620dd95f3e6e9fddfb34701eb8a8846aa61ca1df96
  amctrl.h=298c4a2dca4b6abfd50b0c1cd2fe0574b0fa15ef1d86d55471f420d1ca95e4a5
  bn.c=06d2886369f2490855e9e1541ab84901b4a5b03661fbbcd6899444d18c28718f
  ec.c=f86bc1c13680da95486538b0202467e089a2f50907caebe1afa31d489d96df48
  kirk_common.h=8bc730e60c1a5498cec2f649bf8bc356ce85a843da8d2f4a278f19d14cce17a5
  kirk_engine.c=74e363f78fdec5c0fc82842a21c11c3b0363996a931805cec9fd11231a3e7763
  kirk_engine.h=d827ae841fe160a52ae4ad8f118c6d46476a5a0887d349328419197ad5e996bc)
fetch_pinned_files(libkirk "${KIRK_DIR}" "${KIRK_URL}" "${KIRK_FILES}" KIRK_SOURCES)
add_library(libkirk STATIC ${KIRK_SOURCES})
target_include_directories(libkirk INTERFACE "${KIRK_DIR}")
if(MSVC)
  target_compile_options(libkirk PRIVATE /W0)
  target_compile_definitions(libkirk PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(libkirk PRIVATE -O2 -w)
endif()

# EBOOT decryption (gow_disc --decrypt-eboot): PPSSPP's Core/ELF/PrxDecrypter.cpp (GPL-2.0+, Copyright (c)
# 2012- PPSSPP Project), downloaded from tag v1.20.4 and checked by SHA-256 like libkirk, because it holds
# the PSP's module keys; runtime/prxshim stands in for the PPSSPP headers it includes.
set(PRX_DIR "${GOW_DEPS_DIR}/ppsspp_prx")
set(PRX_URL "https://raw.githubusercontent.com/hrydgard/ppsspp/v1.20.4/Core/ELF")
set(PRX_FILES
  PrxDecrypter.cpp=9bea13a3867b7b7e60b26f648bfcf650ce96e9f0da2203949cf1f9c2df54659b
  PrxDecrypter.h=9021115e71851bc2ae80d37f6532a9ab07990715d8a42478bc7e0bc219bdb9a4)
fetch_pinned_files(ppsspp_prx "${PRX_DIR}/Core/ELF" "${PRX_URL}" "${PRX_FILES}" PRX_SOURCES)
add_library(prx_decrypt STATIC ${PRX_SOURCES} runtime/rt_prx.cpp)
target_include_directories(prx_decrypt PRIVATE "${PRX_DIR}" runtime/prxshim runtime)
target_link_libraries(prx_decrypt PRIVATE libkirk)
set_target_properties(prx_decrypt PROPERTIES CXX_STANDARD 17)
if(MSVC)
  target_compile_options(prx_decrypt PRIVATE /W0)
else()
  target_compile_options(prx_decrypt PRIVATE -O2 -w)
endif()

add_executable(gow_disc runtime/gow_disc.c)
set_target_properties(gow_disc PROPERTIES C_STANDARD 11)
target_include_directories(gow_disc PRIVATE runtime)
target_link_libraries(gow_disc PRIVATE chdr-static prx_decrypt libkirk)
if(MSVC)
  target_compile_definitions(gow_disc PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

if(GOW_TOOLS_ONLY)
  return()
endif()

if(GOW_ANDROID)
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
  function(gow_add_rt name)
    add_library(${name} SHARED ${ARGN} runtime/rt_android.c)
  endfunction()
else()
  function(gow_add_rt name)
    add_executable(${name} ${ARGN})
  endfunction()
endif()
gow_add_rt(gow_rt
  runtime/rt_atrac.c
  runtime/rt_audio.c
  runtime/rt_core.c
  runtime/rt_coro.c
  runtime/rt_debug.c
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
  runtime/rt_savedata.c
  runtime/rt_thread.c
  runtime/rt_vfpu.c
  runtime/rt_selftest.c
  $<$<PLATFORM_ID:Windows>:runtime/gow_rt.rc>
  ${GEN_SOURCES})
set_target_properties(gow_rt PROPERTIES OUTPUT_NAME "gow_rt_${GAME}" C_STANDARD 11)
if(GOW_ANDROID)
  set_target_properties(gow_rt PROPERTIES OUTPUT_NAME "main")
  set_source_files_properties(runtime/rt_main.c PROPERTIES COMPILE_DEFINITIONS "main=gow_main")
  # Do not rely solely on toolchain predefined macros for Android-only SDL safeguards.
  target_compile_definitions(gow_rt PRIVATE GOW_ANDROID=1)
  target_link_options(gow_rt PRIVATE -Wl,--allow-shlib-undefined)
endif()
target_include_directories(gow_rt PRIVATE runtime "${GEN_DIR}")
target_link_libraries(gow_rt PRIVATE chdr-static at3_standalone openh264_dec libkirk)
if(GOW_WINDOW)
  if(GOW_ANDROID)
    add_library(SDL3_android SHARED IMPORTED)
    set_target_properties(SDL3_android PROPERTIES IMPORTED_LOCATION "${GOW_SDL_SO}")
    file(GLOB _sdl_inc "${CMAKE_SOURCE_DIR}/build/deps/SDL3-*/include")
    target_include_directories(gow_rt PRIVATE ${_sdl_inc})
    target_link_libraries(gow_rt PRIVATE SDL3_android)
  else()
    target_link_libraries(gow_rt PRIVATE SDL3::SDL3-static)
  endif()
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
  # Android runs the recompilation and GE command processing on ARM64; use the
  # stronger optimizing pass for the native game/runtime hot paths without
  # enabling fast-math or changing floating-point contraction semantics.
  if(ANDROID)
    target_compile_options(gow_rt PRIVATE -O3 -fno-strict-aliasing -ffp-contract=off -Wno-unused-label)
  else()
    target_compile_options(gow_rt PRIVATE -O2 -fno-strict-aliasing -ffp-contract=off -Wno-unused-label)
  endif()
  target_link_libraries(gow_rt PRIVATE m)
endif()

# MinGW (the launcher's portable llvm-mingw toolchain): link the C/C++ runtimes statically so the game exes run
# without the toolchain's DLLs, and write a PDB (CodeView) like the MSVC build, for --profile's symbol lookup.
if(MINGW)
  target_link_libraries(gow_rt PRIVATE dbghelp winmm)
  target_compile_options(gow_rt PRIVATE -gcodeview)
  target_link_options(gow_rt PRIVATE -static -Wl,--pdb=)
  target_link_options(gow_disc PRIVATE -static)
endif()

# MSBuild compiles one file at a time per project unless /MP is set, and gow_rt only starts compiling once the
# libraries it links are built: SDL3 alone took about 35 s that way. /MP only parallelises, the objects are the same.
if(MSVC)
  foreach(lib SDL3-static chdr-static chdr-lzma miniz zstd at3_standalone openh264_dec libkirk prx_decrypt)
    if(TARGET ${lib})
      target_compile_options(${lib} PRIVATE /MP)
    endif()
  endforeach()
endif()
