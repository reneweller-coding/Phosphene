# Phosphene -- a generator for complete psytrance sets.
#
# Layout:
#   Core/         framework-free engine (static library PhospheneCore)
#   Tools/render  phos_render: offline render, MIDI export, benchmark
#   Tests/        phos_selftest and the vector-path variants of phos_vectest
#   Plugin/       JUCE VST3 + Standalone (Phase 6)
#   Quest/        native OpenXR app (Phase 7)
cmake_minimum_required(VERSION 3.22)
project(Phosphene VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# One source of truth for the version: this project() line. It reaches
#   * the plugin, through juce_add_plugin's VERSION ${PROJECT_VERSION} (Plugin/CMakeLists.txt),
#     which is what a DAW shows and what lands in the Windows version resource;
#   * every C++ translation unit as PHOS_VERSION, which is what `phos_render --version` prints;
#   * the installer and the manifest, because Deploy/build_release.ps1 reads the line back out of
#     this file when it is not given a -Version.
# Nothing else may spell the version out. Noctuary shipped a 1.1.0 with 1.0.0 inside the executable
# because two places held it and only one was changed; the version is checked against the binary in
# Tools/release/check_package.ps1 so that cannot happen silently here.
add_compile_definitions(PHOS_VERSION="${PROJECT_VERSION}")

option(PHOS_BUILD_TOOLS "Build phos_render and the tests" ON)
# The plugin is a desktop artefact and JUCE builds a host-side helper (juceaide) to make it, which
# a cross-compiler cannot produce: configuring for Android with the plugin on fails inside JUCE with
# "No CMAKE_C_COMPILER could be found", a message that says nothing about the real cause. So the
# default follows the platform. Quest/build_apk.ps1 passed -DPHOS_BUILD_PLUGIN=OFF and worked; a
# hand-written Android configure did not, and the error sent the reader into JUCE instead of here.
if(ANDROID)
    option(PHOS_BUILD_PLUGIN "Build the JUCE plugin (VST3 + Standalone)" OFF)
else()
    option(PHOS_BUILD_PLUGIN "Build the JUCE plugin (VST3 + Standalone)" ON)
endif()
option(PHOS_AVX2 "Build the x86-64 core with AVX2 + FMA" ON)
option(PHOS_STATIC_RUNTIME "Link the MSVC runtime statically (for distribution)" OFF)
# A build whose binaries are meant for other machines. It does one thing: it stops Core/CMakeLists.txt
# compiling this checkout's Core/data path into PhospheneCore as the last step of the core's lookup
# for library.phoswt, melody.phosmdl and bass.phosmdl.
#
# That path is a development convenience and a shipping hazard. It puts the build machine's directory
# layout inside every binary anybody downloads and -- far worse -- it makes a missing data file
# invisible *on the build machine*: a staged binary finds the source tree whatever was staged beside
# it, so a package with a model deleted renders exactly like a correct one. That is not a thought
# experiment, it is what the release round of 16.09.2026 measured (Tools/release/check_package.ps1,
# the note above check F: a deleted bass.phosmdl and a flipped byte both went through).
# Deploy/build_release.ps1 sets this ON, ctest runs in that configuration, and the package check then
# catches a missing model the way it always should have.
option(PHOS_SHIP "A build for other machines: no path into this source tree inside the binaries" OFF)
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
# Intel's icx (oneAPI) passes for MSVC here -- it takes MSVC's flags, so if(MSVC) is true -- but it is
# not MSVC underneath: its default floating-point model is "fast", and even under /fp:precise it
# contracts a*b+c into vfmadd (measured 23.09.2026 on a one-line function; /fp:precise alone still
# emitted vfmadd213sd). /Qfma- turns the contraction off, which icx reports as overriding the model's
# default -- that override is the point, so the warning is silenced.
if(CMAKE_CXX_COMPILER_ID STREQUAL "IntelLLVM")
    add_compile_options(/fp:precise /Qfma- /clang:-Wno-overriding-option)
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
    # The probe-written plan fields as bit patterns, and the planning benchmark (round "speed", 20.09.2026).
    add_subdirectory(Tools/plandump)
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
