# Phosphene -- a generator for complete psytrance sets.
#
# Layout:
#   Core/         framework-free engine (static library PhospheneCore)
#   Tools/render  phos_render: offline render, MIDI export, benchmark
#   Tests/        phos_selftest and the vector-path variants of phos_vectest
#   Plugin/       JUCE VST3 + Standalone (Phase 6)
#   Quest/        native OpenXR app (Phase 7)
cmake_minimum_required(VERSION 3.22)
project(Phosphene VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(PHOS_BUILD_TOOLS "Build phos_render and the tests" ON)
option(PHOS_AVX2 "Build the x86-64 core with AVX2 + FMA" ON)
option(PHOS_STATIC_RUNTIME "Link the MSVC runtime statically (for distribution)" OFF)

if(MSVC)
    add_compile_options(/MP)
    add_link_options($<$<CONFIG:Release>:/DEBUG> $<$<CONFIG:Release>:/OPT:REF> $<$<CONFIG:Release>:/OPT:ICF>)
endif()
if(PHOS_STATIC_RUNTIME AND MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# No fast-math, and no floating-point contraction anywhere: the offline render is the determinism
# oracle, and the lane-parallel paths (Core/include/phos/Vec.h) are tested bit for bit against the
# scalar path. Clang contracts a*b+c into a fused multiply-add by default, which would make the
# scalar reference differ from the NEON lanes on the Quest. MSVC does not contract under /fp:precise.
if(NOT MSVC)
    add_compile_options(-ffp-contract=off)
endif()

# Android (the Quest): configured with the NDK toolchain, see Quest/README.md.
#
#   cmake -S . -B build-android -G "Unix Makefiles"
#         -DCMAKE_MAKE_PROGRAM=<ndk>/prebuilt/windows-x86_64/bin/make.exe
#         -DCMAKE_TOOLCHAIN_FILE=<ndk>/build/cmake/android.toolchain.cmake
#         -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DCMAKE_BUILD_TYPE=Release
#
# This builds PhospheneCore, phos_selftest, phos_vectest and phos_render for the device; the app
# itself is its own project under Quest/. The NDK's default STL is c++_static, so the executables
# run from /data/local/tmp without pushing a runtime alongside them.
if(ANDROID)
    # NEON comes with arm64 and needs no flag; AVX2 is meaningless here. Forced into the cache so a
    # dump of it says what was really built rather than the x86 default.
    set(PHOS_AVX2 OFF CACHE BOOL "" FORCE)
    if(NOT ANDROID_ABI STREQUAL "arm64-v8a")
        message(WARNING "ANDROID_ABI=${ANDROID_ABI}: the lane path and the Quest budget assume arm64-v8a")
    endif()
    message(STATUS "Phosphene for Android: ${ANDROID_ABI}, API ${ANDROID_PLATFORM_LEVEL}, NDK ${ANDROID_NDK}")
endif()

add_subdirectory(Core)

if(PHOS_BUILD_TOOLS)
    enable_testing()
    add_subdirectory(Tools/render)
    add_subdirectory(Tests)
endif()
