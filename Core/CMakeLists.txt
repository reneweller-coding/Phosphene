add_library(PhospheneCore STATIC
    src/Params.cpp
    src/Clock.cpp
    src/Score.cpp
    src/Midi.cpp
    src/WavWriter.cpp
    src/Loudness.cpp
    src/Halfband.cpp
    src/Kick.cpp
    src/Bass.cpp
    src/Perc.cpp
    src/Acid.cpp
    src/Poly.cpp
    src/WaveTable.cpp
    src/WaveTableFile.cpp
    src/Reverb.cpp
    src/Dynamics.cpp
    src/Sfx.cpp
    src/SfxBankTables.cpp
    src/PsyFx.cpp
    src/Texture.cpp
    src/Vocal.cpp
    src/Rhythm.cpp
    src/Form.cpp
    src/Melody.cpp
    src/Corpus.cpp
    src/CorpusTables.cpp
    src/Model.cpp
    src/Composer.cpp
    src/Probe.cpp
    src/SetFile.cpp
    src/Engine.cpp
)
target_include_directories(PhospheneCore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
# Where the shipped data lies in the source tree: the wavetable pack and the two learned models of
# Phase 8. It is the *last* place WaveTableFile.cpp and NeuralModel::load() look -- after the host's
# own resource directory (setWaveTableSearchPath() / setModelSearchPath(), which the plugin and the
# Quest app now both set) and the working directory -- so that a development build finds Core/data
# without a copy step.
#
# **Only a development build.** The comment this replaces claimed "a shipped build never depends on
# it", and that was wrong in the one way that mattered: the define was unconditional, so every
# shipped binary carried this machine's absolute path *and used it*. On the build machine that made a
# missing data file unobservable -- the release round of 16.09.2026 deleted a staged bass.phosmdl and
# flipped a byte of a staged melody.phosmdl, and the end-to-end render check passed both times,
# because the binary quietly read Core/data instead. PHOS_SHIP (the root CMakeLists.txt) turns it
# off, Deploy/build_release.ps1 sets PHOS_SHIP, and Tools/release/check_package.ps1 check F2 fails
# the package when the string is in a staged binary anyway.
#
# The tests name Core/data at compile time and must keep doing so in both configurations, so they
# get the same define on their own targets from Tests/CMakeLists.txt.
if(NOT PHOS_SHIP)
    target_compile_definitions(PhospheneCore PUBLIC PHOS_SOURCE_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
endif()
target_compile_features(PhospheneCore PUBLIC cxx_std_20)

# The core's build id for the probe cache (include/phos/Probe.h, cmake/build_id.cmake): a hash of Core/src,
# Core/include and the compile flags, made again by every build (the target is always out of date; the
# script takes 0.1 s and rewrites the header only when the id changed). Per configuration, because Debug
# and Release are two cores. The flags file is written at generate time; what it cannot see -- options
# added to the target after this point -- is named by hand (PHOS_AVX2, PHOS_SHIP). An ASan tree differs in
# CMAKE_CXX_FLAGS (/fsanitize=address), which is in the file.
set(PHOS_BUILD_ID_DIR "${CMAKE_CURRENT_BINARY_DIR}/build_id/$<CONFIG>")
file(GENERATE OUTPUT "${PHOS_BUILD_ID_DIR}/flags.txt" CONTENT
"${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} ${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_PROCESSOR}
config=$<CONFIG> flags=${CMAKE_CXX_FLAGS} $<$<CONFIG:Debug>:${CMAKE_CXX_FLAGS_DEBUG}>$<$<CONFIG:Release>:${CMAKE_CXX_FLAGS_RELEASE}>$<$<CONFIG:RelWithDebInfo>:${CMAKE_CXX_FLAGS_RELWITHDEBINFO}>$<$<CONFIG:MinSizeRel>:${CMAKE_CXX_FLAGS_MINSIZEREL}>
avx2=${PHOS_AVX2} ship=${PHOS_SHIP} options=$<TARGET_PROPERTY:PhospheneCore,COMPILE_OPTIONS> definitions=$<TARGET_PROPERTY:PhospheneCore,COMPILE_DEFINITIONS>
")
add_custom_target(phos_build_id
    COMMAND ${CMAKE_COMMAND} "-DPHOS_CORE_DIR=${CMAKE_CURRENT_SOURCE_DIR}" "-DPHOS_FLAGS_FILE=${PHOS_BUILD_ID_DIR}/flags.txt"
            "-DPHOS_OUT=${PHOS_BUILD_ID_DIR}/phos_build_id.h" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/build_id.cmake"
    BYPRODUCTS "${PHOS_BUILD_ID_DIR}/phos_build_id.h"
    COMMENT "Core build id (probe cache key)"
    VERBATIM)
add_dependencies(PhospheneCore phos_build_id)
target_include_directories(PhospheneCore PRIVATE "${PHOS_BUILD_ID_DIR}")
set_target_properties(PhospheneCore PROPERTIES POSITION_INDEPENDENT_CODE ON)
find_package(Threads)
if(Threads_FOUND)
    target_link_libraries(PhospheneCore PUBLIC Threads::Threads)
endif()
# The cue bridge of PLAN 8.3 (include/phos/Cue.h) is header-only, so whoever includes it needs the
# socket library. PUBLIC on the core, which every consumer already links.
if(WIN32)
    target_link_libraries(PhospheneCore PUBLIC ws2_32)
endif()
if(MSVC)
    target_compile_options(PhospheneCore PRIVATE /W4 /permissive- /utf-8)
    target_compile_definitions(PhospheneCore PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(PhospheneCore PRIVATE -Wall -Wextra)
endif()

# AVX2 with FMA for the lane-parallel paths (Vec.h). PUBLIC, so everything that links the core sees
# the same vector path the core was built with. The arm64 build (Quest) takes NEON on its own.
if(PHOS_AVX2 AND CMAKE_SYSTEM_PROCESSOR MATCHES "(x86_64|AMD64|x64)")
    if(MSVC)
        target_compile_options(PhospheneCore PUBLIC /arch:AVX2)
    else()
        target_compile_options(PhospheneCore PUBLIC -mavx2 -mfma)
    endif()
endif()
