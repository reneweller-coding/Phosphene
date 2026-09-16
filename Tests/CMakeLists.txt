# On Android the executables are cross-compiled for the headset, so ctest must not try to run them
# on the build machine; they go to the device with adb (Quest/README.md).
function(phos_add_test name)
    if(NOT ANDROID)
        add_test(NAME ${name} COMMAND ${ARGN})
    endif()
endfunction()

# The build guard (Tools/release/quest_guard.cmake): the Quest sources still compile for the
# headset, and the root project still configures for Android with its default options. Neither is
# reachable from anything else in this file -- the Quest app is a CMake project of its own and no
# target here builds it -- which is how both broke unnoticed on 16.09.2026. Five seconds measured
# on this machine (3.5 s configure, 1.6 s syntax check) against a suite of about 450, so it is
# cheap enough to stay on.
#
# Only on the build machine: an Android ctest run does not cross-check itself, and a second Android
# toolchain inside one is nonsense. 77 is the exit code the script uses when it cannot run at all
# (no NDK, no ThirdParty headers); SKIP_RETURN_CODE turns that into a visible "Skipped" with the
# reason instead of a red suite for anybody without the NDK.
if(NOT ANDROID)
    add_test(NAME questguard COMMAND ${CMAKE_COMMAND}
             "-DPHOS_ROOT=${CMAKE_SOURCE_DIR}" "-DPHOS_SCRATCH=${CMAKE_BINARY_DIR}/guard"
             -P "${CMAKE_SOURCE_DIR}/Tools/release/quest_guard.cmake")
    set_tests_properties(questguard PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
endif()

# The self test: every building block measured, and the engine as a whole.
add_executable(phos_selftest selftest.cpp)
target_link_libraries(phos_selftest PRIVATE PhospheneCore)
phos_add_test(selftest phos_selftest)

# The lane paths against the scalar reference, bit for bit, once per vector path. The main variant
# is built as the core is (AVX2 on x86-64, NEON on the Quest); on x86 the NEON path also runs through
# neonshim/arm_neon.h, and the scalar path is forced. The variants are header-only apart from the
# half-band design, which they compile themselves: linking PhospheneCore would bring its AVX2 flags.
# The Kaleidoscope bridge demonstrated without ears (PLAN 8.3): the same chain the plugin builds,
# assembled by hand, so the cues of a whole track can be captured and compared with the score. It
# opens a socket, which is why it is not part of the self test; Tools/cue_check.py drives it and ctest
# runs that script. On Android nothing here runs on the build machine anyway (phos_add_test).
add_executable(phos_cuedemo cuedemo.cpp)
target_link_libraries(phos_cuedemo PRIVATE PhospheneCore)
if(NOT ANDROID)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_FOUND)
        add_test(NAME cuecheck COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/Tools/cue_check.py
                 --demo $<TARGET_FILE:phos_cuedemo>)
    endif()
endif()

add_executable(phos_vectest vectest.cpp)
target_link_libraries(phos_vectest PRIVATE PhospheneCore)
phos_add_test(vectest phos_vectest)
set(PHOS_TEST_TARGETS phos_selftest phos_vectest phos_cuedemo)

if(NOT ANDROID AND CMAKE_SYSTEM_PROCESSOR MATCHES "(x86_64|AMD64|x64)")
    if(PHOS_AVX2)
        target_compile_definitions(phos_vectest PRIVATE PHOS_EXPECT_PATH="avx2")
    endif()
    foreach(variant neon scalar)
        # Model.cpp joins the fixed source list of the variants (Phase 8): it is the one file that
        # turns the model's lane kernels into a whole forward pass, and the scalar variant's timing
        # of it is what the Quest's NEON path will resemble. It needs nothing from the core but
        # kCorpusAlphabet, which is a constant in a header.
        add_executable(phos_vectest_${variant} vectest.cpp ${CMAKE_SOURCE_DIR}/Core/src/Halfband.cpp
                       ${CMAKE_SOURCE_DIR}/Core/src/Perc.cpp ${CMAKE_SOURCE_DIR}/Core/src/Params.cpp
                       ${CMAKE_SOURCE_DIR}/Core/src/Poly.cpp ${CMAKE_SOURCE_DIR}/Core/src/WaveTable.cpp
                       ${CMAKE_SOURCE_DIR}/Core/src/WaveTableFile.cpp
                       ${CMAKE_SOURCE_DIR}/Core/src/Model.cpp)
        target_include_directories(phos_vectest_${variant} PRIVATE ${CMAKE_SOURCE_DIR}/Core/include)
        target_compile_definitions(phos_vectest_${variant} PRIVATE PHOS_SOURCE_DATA_DIR="${CMAKE_SOURCE_DIR}/Core/data")
        target_compile_features(phos_vectest_${variant} PRIVATE cxx_std_20)
        phos_add_test(vectest_${variant} phos_vectest_${variant})
        list(APPEND PHOS_TEST_TARGETS phos_vectest_${variant})
    endforeach()
    target_include_directories(phos_vectest_neon BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/neonshim)
    target_compile_definitions(phos_vectest_neon PRIVATE PHOS_NEON_SHIM=1 PHOS_EXPECT_PATH="neon-shim")
    target_compile_definitions(phos_vectest_scalar PRIVATE PHOS_FORCE_SCALAR=1 PHOS_EXPECT_PATH="scalar")
elseif(ANDROID AND CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64")
    target_compile_definitions(phos_vectest PRIVATE PHOS_EXPECT_PATH="neon")
endif()

foreach(t ${PHOS_TEST_TARGETS})
    if(MSVC)
        target_compile_options(${t} PRIVATE /W4 /permissive- /utf-8)
        target_compile_definitions(${t} PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
        # The NDK's clang is stricter than MSVC and finds things MSVC does not; the same warnings
        # the core is built with, so a Quest build does not hide anything the desktop build shows.
        target_compile_options(${t} PRIVATE -Wall -Wextra)
    endif()
endforeach()

# The host contract (Tests/hosttest.cpp): the plugin's side of what a DAW does, plus the oracle --
# the plugin's own clock has to render exactly what phos_render renders. Only built with the plugin,
# because it links the plugin's shared code.
#
# juce_add_plugin makes `Phosphene` the shared-code library (it builds as Phosphene_SharedCode.lib),
# so linking that one target gives the processor, the editor and the JucePlugin_* definitions; the
# JUCE modules are linked to it privately, so the test asks for them again.
if(TARGET Phosphene)
    add_executable(phos_hosttest hosttest.cpp)
    target_link_libraries(phos_hosttest PRIVATE Phosphene PhospheneCore
                          juce::juce_audio_utils juce::juce_recommended_config_flags)
    # An application, not a plugin: it may run a dispatch loop, which is how the editor's timers get
    # their chance while it is open.
    target_compile_definitions(phos_hosttest PRIVATE JUCE_STANDALONE_APPLICATION=1
                               JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_DISPLAY_SPLASH_SCREEN=0 JUCE_REPORT_APP_USAGE=0)
    target_include_directories(phos_hosttest PRIVATE ${CMAKE_SOURCE_DIR}/Plugin)
    if(MSVC)
        target_compile_options(phos_hosttest PRIVATE /W4 /permissive- /utf-8)
        target_compile_definitions(phos_hosttest PRIVATE _CRT_SECURE_NO_WARNINGS)
        # The composer's probe renders put whole engines on the stack.
        target_link_options(phos_hosttest PRIVATE "LINKER:/STACK:16777216")
    endif()
    # The shipped pack beside the test binary, exactly as Plugin/CMakeLists.txt installs it beside
    # the artefacts. Without it the host test could only ever see the development build's
    # PHOS_SOURCE_DATA_DIR, and the plugin's own path resolution -- the thing a user depends on --
    # would go untested.
    add_custom_command(TARGET phos_hosttest POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_SOURCE_DIR}/Core/data/library.phoswt" "$<TARGET_FILE_DIR:phos_hosttest>/library.phoswt"
        VERBATIM)
    add_test(NAME hosttest COMMAND phos_hosttest)
endif()

# The built VST3 loaded the way a DAW loads it (Tests/vst3test.cpp): the module off the disk, the
# factory, an instance, a transport, the state and the editor. It is the part of pluginval that can
# live in the repository -- pluginval itself is not on this machine (docs/PLAN.md, Phase 6).
if(TARGET Phosphene_VST3)
    add_executable(phos_vst3test vst3test.cpp)
    target_link_libraries(phos_vst3test PRIVATE juce::juce_audio_utils juce::juce_recommended_config_flags)
    # A modal loop is allowed here: a VST3 tells its controller about a restored state through the
    # message queue, and a test with no window has to run that queue by hand to see the result.
    target_compile_definitions(phos_vst3test PRIVATE JUCE_STANDALONE_APPLICATION=1 JUCE_PLUGINHOST_VST3=1
                               JUCE_MODAL_LOOPS_PERMITTED=1
                               JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_DISPLAY_SPLASH_SCREEN=0 JUCE_REPORT_APP_USAGE=0)
    add_dependencies(phos_vst3test Phosphene_VST3)
    if(MSVC)
        target_compile_options(phos_vst3test PRIVATE /W4 /permissive- /utf-8)
        target_compile_definitions(phos_vst3test PRIVATE _CRT_SECURE_NO_WARNINGS)
        target_link_options(phos_vst3test PRIVATE "LINKER:/STACK:16777216")
    endif()
    # The module inside the .vst3 bundle: on Windows that file is what the host loads, and it is
    # the one CMake can name (the bundle itself is a directory made by a post-build step).
    add_test(NAME vst3test COMMAND phos_vst3test "$<TARGET_FILE:Phosphene_VST3>")
endif()
