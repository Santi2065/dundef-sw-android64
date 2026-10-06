cmake_minimum_required(VERSION 3.20)
project(dundef_port CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# dynarmic: only the A32 frontend, bundled externals, headers-only boost
set(DYNARMIC_FRONTENDS "A32" CACHE STRING "" FORCE)
set(DYNARMIC_TESTS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_USE_BUNDLED_EXTERNALS ON CACHE BOOL "" FORCE)
set(DYNARMIC_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_USE_PRECOMPILED_HEADERS OFF CACHE BOOL "" FORCE)
set(Boost_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/boost_1_86_0)
add_subdirectory(third_party/dynarmic EXCLUDE_FROM_ALL)

add_library(DunDef-Android SHARED
    src/memory.cpp
    src/cpu.cpp
    src/libc.cpp
    src/gl.cpp
    src/jni.cpp
    third_party/dlmalloc/dlmalloc.c)
target_compile_options(DunDef-Android PRIVATE -O2 -Wall -Wno-unused-parameter)
set_source_files_properties(third_party/dlmalloc/dlmalloc.c PROPERTIES COMPILE_OPTIONS "-w")
target_link_libraries(DunDef-Android PRIVATE dynarmic log GLESv2 EGL dl z)
target_link_options(DunDef-Android PRIVATE -Wl,-z,max-page-size=16384 -Wl,--exclude-libs,ALL)
