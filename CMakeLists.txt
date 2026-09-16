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
option(PHOS_BUILD_PLUGIN "Build the JUCE plugin (VST3 + Standalone)" ON)
option(PHOS_AVX2 "Build the x86-64 core with AVX2 + FMA" ON)
option(PHOS_STATIC_RUNTIME "Link the MSVC runtime statically (for distribution)" OFF)
set(PHOS_JUCE_TAG "9.0.1" CACHE STRING "JUCE git tag to fetch when ThirdParty/JUCE is absent")

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
endif()

# The plugin (Phase 6). JUCE is taken from ThirdParty/JUCE when it is there -- that directory is
# ignored by git and may be a copy of a JUCE checkout a sibling build already has -- and fetched
# from GitHub otherwise. Nothing under Core/ ever includes JUCE; the dependency stops here.
if(PHOS_BUILD_PLUGIN)
    include(FetchContent)
    set(JUCE_BUILD_EXTRAS OFF CACHE BOOL "" FORCE)
    set(JUCE_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/JUCE/CMakeLists.txt)
        set(FETCHCONTENT_SOURCE_DIR_JUCE ${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/JUCE CACHE PATH "" FORCE)
    endif()
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG        ${PHOS_JUCE_TAG}
        GIT_SHALLOW    TRUE
        GIT_PROGRESS   TRUE)
    FetchContent_MakeAvailable(JUCE)
    add_subdirectory(Plugin)
endif()

# After the plugin, so the host test can link the plugin's shared code.
if(PHOS_BUILD_TOOLS)
    add_subdirectory(Tests)
endif()
