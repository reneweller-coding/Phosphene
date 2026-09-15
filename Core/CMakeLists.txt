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
    src/Composer.cpp
    src/Engine.cpp
)
target_include_directories(PhospheneCore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(PhospheneCore PUBLIC cxx_std_20)
set_target_properties(PhospheneCore PROPERTIES POSITION_INDEPENDENT_CODE ON)
find_package(Threads)
if(Threads_FOUND)
    target_link_libraries(PhospheneCore PUBLIC Threads::Threads)
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
