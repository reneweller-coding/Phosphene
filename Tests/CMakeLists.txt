# The self test: every building block measured, and the engine as a whole.
add_executable(phos_selftest selftest.cpp)
target_link_libraries(phos_selftest PRIVATE PhospheneCore)
add_test(NAME selftest COMMAND phos_selftest)

# The lane paths against the scalar reference, bit for bit, once per vector path. The main variant
# is built as the core is (AVX2 on x86-64, NEON on the Quest); on x86 the NEON path also runs through
# neonshim/arm_neon.h, and the scalar path is forced. The variants are header-only apart from the
# half-band design, which they compile themselves: linking PhospheneCore would bring its AVX2 flags.
add_executable(phos_vectest vectest.cpp)
target_link_libraries(phos_vectest PRIVATE PhospheneCore)
add_test(NAME vectest COMMAND phos_vectest)
set(PHOS_TEST_TARGETS phos_selftest phos_vectest)

if(NOT ANDROID AND CMAKE_SYSTEM_PROCESSOR MATCHES "(x86_64|AMD64|x64)")
    if(PHOS_AVX2)
        target_compile_definitions(phos_vectest PRIVATE PHOS_EXPECT_PATH="avx2")
    endif()
    foreach(variant neon scalar)
        add_executable(phos_vectest_${variant} vectest.cpp ${CMAKE_SOURCE_DIR}/Core/src/Halfband.cpp
                       ${CMAKE_SOURCE_DIR}/Core/src/Perc.cpp ${CMAKE_SOURCE_DIR}/Core/src/Params.cpp
                       ${CMAKE_SOURCE_DIR}/Core/src/Poly.cpp ${CMAKE_SOURCE_DIR}/Core/src/WaveTable.cpp)
        target_include_directories(phos_vectest_${variant} PRIVATE ${CMAKE_SOURCE_DIR}/Core/include)
        target_compile_features(phos_vectest_${variant} PRIVATE cxx_std_20)
        add_test(NAME vectest_${variant} COMMAND phos_vectest_${variant})
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
    add_test(NAME hosttest COMMAND phos_hosttest)
endif()
