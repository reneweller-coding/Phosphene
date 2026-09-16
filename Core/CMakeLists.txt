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
    src/Rhythm.cpp
    src/Form.cpp
    src/Melody.cpp
    src/Corpus.cpp
    src/CorpusTables.cpp
    src/Model.cpp
    src/Composer.cpp
    src/SetFile.cpp
    src/Engine.cpp
)
target_include_directories(PhospheneCore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
# Phase 8: where the neural melody model lives in the source tree. It is the *last* place
# NeuralModel::load() looks -- after the host's own resource directory (setModelSearchPath(), which
# the plugin and the Quest app set) and the working directory -- so that a development build and the
# self test find Core/data/ without a copy step, while a shipped build never depends on it.
target_compile_definitions(PhospheneCore PUBLIC PHOS_SOURCE_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
target_compile_features(PhospheneCore PUBLIC cxx_std_20)
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
