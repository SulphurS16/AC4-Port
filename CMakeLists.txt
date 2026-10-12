cmake_minimum_required(VERSION 3.25)
project(ac4 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_definitions(_WIN32_WINNT=0x0A00 WINVER=0x0A00 NOMINMAX UNICODE _UNICODE)

# Self-contained executables (no libc++/libunwind DLLs next to them).
if(MINGW)
  add_link_options(-static)
endif()

# ---- third party ----
file(GLOB ZLIB_SRC ${CMAKE_SOURCE_DIR}/third_party/zlib/*.c)
add_library(ac4_zlib STATIC ${ZLIB_SRC})
target_include_directories(ac4_zlib PUBLIC ${CMAKE_SOURCE_DIR}/third_party/zlib)
target_compile_options(ac4_zlib PRIVATE -w)

# ---- common ----
add_library(ac4_common STATIC
  src/common/bytes.cpp
  src/common/crypto.cpp
  src/common/file.cpp
  src/common/sfo.cpp)
target_include_directories(ac4_common PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(ac4_common PUBLIC ac4_zlib bcrypt)
target_compile_options(ac4_common PRIVATE -Wall -Wextra)

# ---- PPU decoding (shared by recompiler and runtime) ----
add_library(ac4_ppu STATIC src/ppu/ppu_decode.cpp)
target_link_libraries(ac4_ppu PUBLIC ac4_common)
target_compile_options(ac4_ppu PRIVATE -Wall -Wextra)

# ---- SPU ISA, chunk discovery (shared by the SPU recompiler and the runtime) ----
add_library(ac4_spu STATIC src/spu/spu_isa.cpp)
target_link_libraries(ac4_spu PUBLIC ac4_common)
target_compile_options(ac4_spu PRIVATE -Wall -Wextra)

add_executable(ac4-spurecomp src/spurecomp/main.cpp)
target_link_libraries(ac4-spurecomp PRIVATE ac4_spu)
target_compile_options(ac4-spurecomp PRIVATE -Wall -Wextra)

# ---- recompiler ----
add_executable(ac4-recomp
  src/recomp/main.cpp
  src/recomp/image.cpp
  src/recomp/analysis.cpp
  src/recomp/emit.cpp)
target_link_libraries(ac4-recomp PRIVATE ac4_ppu)
target_compile_options(ac4-recomp PRIVATE -Wall -Wextra)

# ---- generated PPU code (game-derived, from local/gen/ppu; never distributed) ----
set(AC4_GEN_DIR "${CMAKE_SOURCE_DIR}/local/gen/ppu" CACHE PATH "Directory with ac4-recomp output")
file(GLOB AC4_GEN_SRC CONFIGURE_DEPENDS "${AC4_GEN_DIR}/*.cpp")
set(AC4_CPU_FLAGS -march=haswell)  # AVX2 + FMA + BMI2 (x86-64-v3)

if(AC4_GEN_SRC)
  add_library(ac4_ppu_gen STATIC ${AC4_GEN_SRC})
  target_include_directories(ac4_ppu_gen PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_compile_options(ac4_ppu_gen PRIVATE -O2 ${AC4_CPU_FLAGS} -fno-strict-aliasing -w)

  file(GLOB AC4_RT_SRC CONFIGURE_DEPENDS
    ${CMAKE_SOURCE_DIR}/src/runtime/*.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/hle/*.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/lv2/*.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/rsx/*.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/gpu/*.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/audio/*.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/media/*.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/game/*.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/spu/*.cpp)
  # Recompiled SPU code (game-derived, from local/gen/spu; never distributed).
  set(AC4_SPU_GEN_DIR "${CMAKE_SOURCE_DIR}/local/gen/spu" CACHE PATH "Directory with ac4-spurecomp output")
  file(GLOB AC4_SPU_GEN_SRC CONFIGURE_DEPENDS "${AC4_SPU_GEN_DIR}/*.cpp")
  if(NOT AC4_SPU_GEN_SRC)
    set(AC4_SPU_GEN_SRC ${CMAKE_SOURCE_DIR}/src/spu/spu_gen_empty.cpp)
  endif()
  add_library(ac4_spu_gen STATIC ${AC4_SPU_GEN_SRC})
  target_include_directories(ac4_spu_gen PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_compile_options(ac4_spu_gen PRIVATE -O2 ${AC4_CPU_FLAGS} -fno-strict-aliasing -w)

  add_executable(ac4 ${AC4_RT_SRC})
  target_include_directories(ac4 PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_compile_options(ac4 PRIVATE ${AC4_CPU_FLAGS} -fno-strict-aliasing -Wall -Wextra -Wno-unused-parameter -g)
  # Whole-archive: the function table references everything anyway; keeps HLE registrars alive.
  target_link_libraries(ac4 PRIVATE ac4_ppu_gen ac4_spu_gen ac4_spu ac4_ppu ac4_common d3d11 dxgi d3dcompiler ole32 psapi
                        synchronization)
endif()

# ---- preparation tool ----
add_executable(ac4-prep
  src/prep/main.cpp
  src/prep/pkg.cpp
  src/prep/self.cpp
  src/prep/firmware.cpp
  src/prep/prx.cpp
  src/prep/sfo.cpp)
target_link_libraries(ac4-prep PRIVATE ac4_common)
target_compile_options(ac4-prep PRIVATE -Wall -Wextra)

# ---- media decoder test tool (development: checks the MPEG-2 decoder on the user's movies) ----
add_executable(ac4-vdectest src/tools/vdectest.cpp src/runtime/media/mpeg2_video.cpp src/runtime/gpu/png.cpp)
target_include_directories(ac4-vdectest PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(ac4-vdectest PRIVATE ac4_common mfplat mfuuid ole32 winmm)
target_compile_options(ac4-vdectest PRIVATE ${AC4_CPU_FLAGS} -Wall -Wextra -Wno-unused-parameter -g)

# ---- texture conversion test (fast paths against the per-texel path, random data) ----
add_executable(ac4-textest src/tools/textest.cpp src/runtime/gpu/rsx_texture.cpp)
target_include_directories(ac4-textest PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(ac4-textest PRIVATE ac4_common)
target_compile_options(ac4-textest PRIVATE ${AC4_CPU_FLAGS} -Wall -Wextra -Wno-unused-parameter -O2)
