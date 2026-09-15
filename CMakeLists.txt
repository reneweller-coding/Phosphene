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

add_subdirectory(Core)

if(PHOS_BUILD_TOOLS)
    enable_testing()
    add_subdirectory(Tools/render)
    add_subdirectory(Tests)
endif()
