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

# The three files the core opens by bare name. A test executable is not an installed product and has
# no resource directory of its own, so it is given what a portable installation has: the files beside
# the binary, and a working directory that is that directory. That works in both configurations --
# with PHOS_SHIP the core has no compiled-in Core/data to fall back on, and without it the fallback
# is simply never reached -- which is the point: the suite must measure the same thing in the
# configuration that ships as in the one worked in.
set(PHOS_DATA_FILES library.phoswt melody.phosmdl bass.phosmdl voices.phosvx)

# Under PHOS_SHIP the core withholds PHOS_SOURCE_DATA_DIR (Core/CMakeLists.txt), but the tests name
# Core/data at compile time -- vectest opens melody.phosmdl by full path, selftest's modelPath()
# builds every reference case's path from it. They are development tools and may know where the
# sources are; the library may not.
function(phos_test_data_dir target)
    if(PHOS_SHIP)
        target_compile_definitions(${target} PRIVATE PHOS_SOURCE_DATA_DIR="${CMAKE_SOURCE_DIR}/Core/data")
    endif()
endfunction()

# Copies the shipped data beside a test binary, the way Plugin/CMakeLists.txt copies it beside the
# artefacts.
function(phos_copy_data target)
    foreach(f ${ARGN})
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${CMAKE_SOURCE_DIR}/Core/data/${f}" "$<TARGET_FILE_DIR:${target}>/${f}"
            VERBATIM)
    endforeach()
endfunction()

# The probe cache of the whole suite (round "speed", 20.09.2026; Core/include/phos/Probe.h): one directory
# in the build tree, shared by every test process, so that a plan one section has measured -- the listening
# seed's first tracks are planned by a dozen sections -- is not rendered again by the next, in this run or a
# later one. Entries are keyed by the core's build id, so a changed core never reads an old core's numbers;
# `cmake -E rm -rf <build>/probe_cache` makes the next run a cold one (it only costs time).
set(PHOS_PROBE_CACHE_DIR "${CMAKE_BINARY_DIR}/probe_cache")

# The self test: every building block measured, and the engine as a whole.
# selftest_probe.cpp: the sections of round "speed" (20.09.2026), registered in selftest.cpp's main() like the rest.
add_executable(phos_selftest selftest.cpp selftest_probe.cpp
    selftest_basics.cpp selftest_lowend.cpp selftest_synths.cpp selftest_mix.cpp selftest_composer.cpp selftest_arrangement.cpp selftest_mixguide.cpp selftest_modulation.cpp)
target_link_libraries(phos_selftest PRIVATE PhospheneCore)
phos_test_data_dir(phos_selftest)
phos_copy_data(phos_selftest ${PHOS_DATA_FILES})
# One ctest test per section (selftest.<name>, labels quick/slow) plus the old all-in-one `selftest`
# (label full, disabled unless PHOS_SELFTEST_FULL is set). The list is not written here: ctest reads
# Tests/selftest_tests.cmake each time it starts, and that asks the built binary for its run table
# (`phos_selftest --list`), so a section added to main() is a test without anyone touching this file.
# The generated file below only tells it where this configuration's binary is.
#
# Every test runs from beside its own binary: that is where the copies above are, and the bare-name
# loads (sharedMelodyModel, sharedBassModel, loadWaveTableLibrary) then find them through the working
# directory -- the same step of the lookup a portable installation uses. It also keeps the one
# relative file the suite writes, phos_selftest_tmp.wav, out of whatever directory ctest was started
# in.
#
# Recommended call, capped so the machine stays usable (24 threads here; six busy cores leave the
# rest to the user and to a build):
#     ctest --test-dir build -C Release -j 6 --output-on-failure
#     ctest --test-dir build -C Release -L quick -j 6      (smoke run, about 15 s)
if(NOT ANDROID)
    get_property(phos_multi GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
    if(phos_multi)
        set(phos_st_file "${CMAKE_CURRENT_BINARY_DIR}/selftest_tests-$<CONFIG>.cmake")
    else()
        set(phos_st_file "${CMAKE_CURRENT_BINARY_DIR}/selftest_tests-single.cmake")
    endif()
    file(GENERATE OUTPUT "${phos_st_file}" CONTENT
"set(PHOS_SELFTEST_EXE [==[$<TARGET_FILE:phos_selftest>]==])
set(PHOS_SELFTEST_DIR [==[$<TARGET_FILE_DIR:phos_selftest>]==])
set(PHOS_TEST_TMP [==[${CMAKE_CURRENT_BINARY_DIR}/tmp/$<CONFIG>]==])
set(PHOS_PROBE_CACHE_DIR [==[${PHOS_PROBE_CACHE_DIR}]==])
set(PHOS_CMAKE [==[${CMAKE_COMMAND}]==])
set(PHOS_SELFTEST_SOURCE [==[${CMAKE_CURRENT_SOURCE_DIR}/selftest.cpp]==])
include([==[${CMAKE_CURRENT_SOURCE_DIR}/selftest_tests.cmake]==])
")
    # ctest knows the configuration only when it runs (-C), so the include picks the file then.
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/selftest_include.cmake"
"if(EXISTS [==[${CMAKE_CURRENT_BINARY_DIR}/selftest_tests-single.cmake]==])
    include([==[${CMAKE_CURRENT_BINARY_DIR}/selftest_tests-single.cmake]==])
elseif(EXISTS \"${CMAKE_CURRENT_BINARY_DIR}/selftest_tests-\${CTEST_CONFIGURATION_TYPE}.cmake\")
    include(\"${CMAKE_CURRENT_BINARY_DIR}/selftest_tests-\${CTEST_CONFIGURATION_TYPE}.cmake\")
else()
    add_test(selftest.NO_CONFIG [==[${CMAKE_COMMAND}]==] -E echo \"no configuration: run ctest with -C Release\")
    set_tests_properties(selftest.NO_CONFIG PROPERTIES WILL_FAIL TRUE)
endif()
")
    set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES "${CMAKE_CURRENT_BINARY_DIR}/selftest_include.cmake")
endif()

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
phos_test_data_dir(phos_cuedemo)
if(NOT ANDROID)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_FOUND)
        add_test(NAME cuecheck COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/Tools/cue_check.py
                 --demo $<TARGET_FILE:phos_cuedemo>)
        # phos_cuedemo inherits it from the script.
        set_tests_properties(cuecheck PROPERTIES ENVIRONMENT "PHOS_PROBE_CACHE=${PHOS_PROBE_CACHE_DIR}")
    endif()
endif()

add_executable(phos_vectest vectest.cpp)
target_link_libraries(phos_vectest PRIVATE PhospheneCore)
phos_test_data_dir(phos_vectest)
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
    # The shipped data beside the test binary, exactly as Plugin/CMakeLists.txt installs it beside
    # the artefacts: the pack and, since the models reached the plugin, the two .phosmdl as well.
    # Without them the host test could only ever see the development build's PHOS_SOURCE_DATA_DIR,
    # and the plugin's own path resolution -- the thing a user depends on -- would go untested.
    #
    # Deliberately NOT phos_test_data_dir(): the host test asks `#if defined(PHOS_SOURCE_DATA_DIR)`
    # to know which build it is in -- a development one, where a plugin with nothing beside it still
    # finds this checkout, or a shipping one, where it falls back and has to say so. Handing it the
    # define would answer that question wrongly. It names no path of its own, so it needs none.
    phos_copy_data(phos_hosttest ${PHOS_DATA_FILES})
    # Two tests since 20.09.2026 (round "speed"; the comment in hosttest.cpp's main()): `--part rest` and
    # `--part realhost`. Every check is in exactly one of them (115 + 4 = the 119 of a run without --part).
    add_test(NAME hosttest COMMAND phos_hosttest --part rest)
    add_test(NAME hosttest.realhost COMMAND phos_hosttest --part realhost)
    # Never under PHOS_MUTE (it measures what the plugin sounds like; muted, it would test silence),
    # whatever the shell that starts ctest has set.
    #
    # `hosttest` checks real-time behaviour -- a set starts within 30 s, no gap over 150 ms while it
    # plays, a timeline tick under 4 ms, faster than real time. It used to run nearly alone: PROCESSORS 5
    # and a RESOURCE_LOCK it shared with vst3test, reserved when this part still ran 9 to 15 minutes and
    # RUN_SERIAL (the first choice) cost 565 s of wall time with nothing else running at all.
    #
    # Re-examined 20.09.2026 (round "test-speed-rest", A6), now that the "speed"/"test-split" rounds'
    # cache and section split already bring this part down to about 85 s alone. Run beside phos_vst3test
    # alone (both started at once, own processes, no other load): plan 15.0 s (limit 30), tick
    # 0.39-0.45 ms (limit 4), 6.7-6.8x real time, 118/118 checks green, twice -- the two do not compete
    # for anything that matters, so the RESOURCE_LOCK that kept them serial on wider -j is gone.
    # PROCESSORS itself is NOT lowered, though, on evidence from trying it: at PROCESSORS 2 (and with
    # this round's five parallelised selftest sections properly declaring the threads they use --
    # Tests/selftest_tests.cmake's PHOS_SELFTEST_PROCESSORS table, added for exactly this reason) a
    # `ctest -j 12` still managed to start hosttest beside hosttest.realhost, testVariety.levelMatch and
    # all four testModalInterchange.presenceArc* parts at once (their own PROCESSORS, 3 + 2 + 4x6, only
    # totals 29 of the machine's 24 hardware threads once hosttest's own 2 are added -- still an
    # oversubscription ctest's slot model does not see, because none of those figures know about each
    # other's real CPU cost, only their own). Result: plan time 31.4 s, over the 30 s bound -- the one
    # contention failure this round saw, first FAIL then fixed by reverting to PROCESSORS 5 here (proof,
    # not assumption: the number that was measured safe originally is kept). Five slots is conservative
    # on this 24-thread machine but it is what is proven, and the RESOURCE_LOCK removal alone already
    # lets hosttest and vst3test overlap instead of forming a serial tail. docs/rounds/2026-09.md (this round's
    # block) has the full readings, including the failing run.
    set_tests_properties(hosttest PROPERTIES LABELS "audio;slow" PROCESSORS 5
                         COST 120 ENVIRONMENT "PHOS_PROBE_CACHE=${PHOS_PROBE_CACHE_DIR}"
                         ENVIRONMENT_MODIFICATION "PHOS_MUTE=unset:")
    # `hosttest.realhost` is offline and times nothing: no lock. Three slots: its render thread, the thread
    # that writes parameters without pause, and the probe workers of the plans those writes keep forcing
    # (eight for half a second per plan, then two, then one -- Composer::measureTrack). No probe cache: its
    # parameters are random, so no entry would ever be read again, and every run would leave some hundred
    # files behind.
    set_tests_properties(hosttest.realhost PROPERTIES LABELS "audio;slow" PROCESSORS 3
                         COST 400 ENVIRONMENT_MODIFICATION "PHOS_MUTE=unset:;PHOS_PROBE_CACHE=unset:")
endif()

# The built VST3 loaded the way a DAW loads it (Tests/vst3test.cpp): the module off the disk, the
# factory, an instance, a transport, the state and the editor. It is the part of pluginval that can
# live in the repository -- pluginval itself is not on this machine (docs/rounds/2026-09.md, Phase 6).
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
    # Like the host test: it checks that the plugin sounds (peak > 0.05), offline and at the rate a
    # device asks for blocks, so it must not be muted. Its own real-time pass (vst3test.cpp) only paces
    # a loop with sleep_until and reads the peak afterwards -- unlike hosttest it asserts no wall-clock
    # deadline of its own, so it is the less fragile of the two. It shared hosttest's PROCESSORS 5 and
    # RESOURCE_LOCK for the same reason (kept apart from hosttest on wider -j); round "test-speed-rest"
    # (20.09.2026, docs/rounds/2026-09.md) measured both running at once, twice, with hosttest's own margins
    # unmoved (see hosttest's comment above) and vst3test itself 36/36 green both times, so the
    # RESOURCE_LOCK is gone -- they no longer form a serial tail. PROCESSORS stays 5: lowering it (tried
    # on hosttest, see its comment) let ctest pack this and several other tests' own worker threads past
    # what the machine has, and vst3test's own scan/load path was never measured under that.
    set_tests_properties(vst3test PROPERTIES LABELS "audio;slow" PROCESSORS 5
                         COST 60 ENVIRONMENT_MODIFICATION "PHOS_MUTE=unset:")
endif()

# Labels for everything that is not a self-test section (those are labelled in selftest_tests.cmake):
# `quick` for what takes seconds -- the vector paths (4 to 8 s under `-j 6`) and the Quest guard (5 s
# alone) -- `slow` for the cue check (32 s: two renders and a real-time pass over a socket; its
# latency limits held under `-j 6` with p95 0.76 ms against 16.7, so it needs no solitude).
if(NOT ANDROID)
    foreach(t vectest vectest_neon vectest_scalar questguard)
        if(TEST ${t})
            set_tests_properties(${t} PROPERTIES LABELS quick)
        endif()
    endforeach()
    if(TEST cuecheck)
        set_tests_properties(cuecheck PROPERTIES LABELS slow)
    endif()
    # The mix guide's render checks (25.09.2026, Tools/mix_audit.py): two styles rendered with stems, measured, held to
    # the bounds the tool writes down with their reasons, the renders deleted again (they are gigabytes).
    if(Python3_FOUND AND TARGET phos_render)
        add_test(NAME mixaudit COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/Tools/mix_audit.py
                 --render $<TARGET_FILE:phos_render> --out ${CMAKE_BINARY_DIR}/mixaudit --fresh --check --clean --styles 1,3)
        set_tests_properties(mixaudit PROPERTIES LABELS slow PROCESSORS 2)
    endif()
endif()

# The plan snapshots (23.09.2026, round "Schnappschuss"): one test per Tests/golden/*.txt, each running
# `phos_plandump --decisions` with the arguments the file names and comparing with the rest of it
# (Tests/plan_snapshot.cmake). A fraction of a second each: no probe renders (level match, auto gain and
# presence match are off in every snapshot), only the plans and the composed score. A meant change is
# written back with Tools/update_snapshots.py, and the golden diff goes into the commit.
if(NOT ANDROID AND TARGET phos_plandump)
    file(GLOB _phos_goldens CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/golden/*.txt")
    foreach(_g ${_phos_goldens})
        get_filename_component(_gn "${_g}" NAME_WE)
        add_test(NAME plansnapshot.${_gn} COMMAND ${CMAKE_COMMAND} "-DPLANDUMP=$<TARGET_FILE:phos_plandump>" "-DGOLDEN=${_g}"
                 -P "${CMAKE_CURRENT_SOURCE_DIR}/plan_snapshot.cmake")
        set_tests_properties(plansnapshot.${_gn} PROPERTIES LABELS quick ENVIRONMENT "PHOS_MUTE=1")
    endforeach()
endif()
