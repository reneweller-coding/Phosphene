# The self test as one ctest test per section -- read by ctest, not by cmake.
#
# Tests/CMakeLists.txt hands this file to ctest through the directory property TEST_INCLUDE_FILES
# (by way of a small generated file that sets the four variables below first). ctest runs it every
# time it starts, before it selects anything, so the list is taken from the binary that is about to
# run -- the same idea as gtest_discover_tests(DISCOVERY_MODE PRE_TEST):
#
#   PHOS_SELFTEST_EXE   the built phos_selftest
#   PHOS_SELFTEST_DIR   its directory: the working directory of every test, where the data files are
#   PHOS_TEST_TMP       a directory each test gets a TMP/TEMP of its own under
#   PHOS_PROBE_CACHE_DIR  the suite's probe cache (Tests/CMakeLists.txt; round "speed", 20.09.2026)
#   PHOS_CMAKE          the cmake that configured the tree (for the placeholder tests)
#   PHOS_SELFTEST_SOURCE  Tests/selftest.cpp, whose run(...) lines the list is compared with
#
# The run("name", fn) table in Tests/selftest.cpp's main() is the only list of sections.
# `phos_selftest --list` prints it; every name becomes `selftest.<name>`, running
# `phos_selftest --only <name>`. A section added there is a test here the next time ctest starts,
# with nobody editing CMake -- which was the requirement, because another round adds sections while
# this one is being written. A section split into parts (19.09.2026) has one run line per part,
# named `group.part` -- hence the dot in the name patterns below -- and so one test per part.
#
# What stays here is only *metadata*: the measured seconds of each section (below). A section the
# table has and this list does not is labelled `slow` until somebody measures it, so `ctest -L quick`
# never turns into a long run by accident; a name here that the table no longer has is reported.

# Seconds of each section (each in its own process; since 20.09.2026, round "speed", from a `ctest -C Release
# -j 8` with an *empty* probe cache and eight probe threads, i9-12900K, Release, another round's builds and
# tests beside it -- with the cache filled the planning sections take a fraction, see docs/PLAN.md; the
# table keeps the cold numbers because they decide what `quick` may contain; four sections that found their
# plans already cached by an earlier test of that run and took 0.4 s keep their uncached 23 to 43 s). They
# become each test's COST, so
# a fresh tree starts the long ones first and the short ones fill the gaps (after one run ctest keeps
# its own costs in Testing/Temporary/CTestCostData.txt); and they decide the label: up to
# PHOS_QUICK_SECONDS is `quick`, above is `slow`. Re-measure when a section moves a lot; nothing
# breaks if a number is stale except the label and the order.
set(PHOS_QUICK_SECONDS 10)
set(PHOS_SELFTEST_SECONDS
    # Split sections (19.09.2026, round "test-split"), named group.part: from `ctest -C Release -j 6` in
    # the plugin build, the machine ~36 % busy on average (another round building and testing beside it).
    testVoices.counterSoundListening   236.6
    testVoices.counterSound77          242.4
    testVoices.counterSound2026        234.5
    testVoices.sound                    61.4
    # The ten entries below (round "test-speed-rest", 20.09.2026, A6): re-measured after the five
    # sections' whole-track renders moved onto phos::probe::runAll's pool (Tests/selftest.cpp),
    # from the round's own final `ctest -C Release -j 12` (commit 8e3cc79, 113 tests, full suite
    # beside them, not isolated) -- docs/PLAN.md has the before numbers and the isolated-run figures.
    # presenceArc{On,Off}{1,2} had never been costed before (they ran with the 600 s "unmeasured"
    # placeholder): this is their first entry.
    testPhaseLock.lock                  38.65
    testModalInterchange.presenceOn1    38.71
    testModalInterchange.presenceOn2    39.53
    testModalInterchange.presenceOff1   41.44
    testModalInterchange.presenceOff2   40.61
    testModalInterchange.presenceArcOn1   86.03
    testModalInterchange.presenceArcOn2   85.14
    testModalInterchange.presenceArcOff1  82.60
    testModalInterchange.presenceArcOff2  79.09
    testVariety.levelMatch             121.09
    testMelody.blockSize               35.7
    testPhaseLock.onsets               66.4
    testAcidVoicing.corners             43.1
    testFoundation.score                42.9
    testGenreRules.listeningSeed        43.1
    testFoundation.render              40.5
    testAcidVoicing.engine             33.6
    testVoices.droneRender             31.0
    testMelody.depthRender             24.3
    testGenreRules.arpGate              22.9
    testVariety.recipes                14.3
    testModalInterchange.newTone       11.6
    testGenreRules.rules               1.5
    testVariety.plans                  1.4
    testMelody.score                   1.1
    testAcidVoicing.night              1.0
    testVoices.acidRide                0.7
    testMelody.variety                 0.7
    testModalInterchange.bass          0.5
    testVoices.score                   0.4
    testModalInterchange.modes         0.0
    testMelody.midi                    0.0
    # Whole sections.
    # Round "speed" (20.09.2026): both switch the probe cache off for their references, so they cost the same warm.
    testProbeSchedule        108.8
    testProbeCache           62.6
    testMixBalance           124.2
    testStereoWidth           83.0
    testMaster              103.7
    testSfxLevel             59.5
    testKickBody             35.3
    testModeColour           41.9
    testEngine               39.7
    testBassRhythm           69.4
    testSectionRules         65.8
    testBandLimit            20.5
    testCues                 14.0
    testAcidColour           8.0
    testMelodyModelWiring    7.6
    testRecipeSpread         6.6
    testPsychedelia          6.4
    testTensionCurve         4.0
    testGateAndDuck          3.2
    testModelDecode          3.2
    testBassBite             2.1
    testArpPatterns          2.3
    testBassModel            2.1
    testForm                 1.8
    testSetArc               0.3
    testRatings              0.1
    testMidiMap              0.1
    testGallery              0.2
    testSoloTrack            0.5
    testPreferences          1.0
    testKnobFuzz            18.0
    testStems               16.6
    testAudibility           8.0
    testAudibilityMatch     70.0
    testSampler              1.5
    testWaveTableQuality     1.3
    testArrangeDynamics      1.0
    testMotifOperators       1.3
    testSfx                  1.0
    testModelFile            1.3
    testWaveTableLibrary     0.8
    testRhythm               0.9
    testPoly                 0.6
    testCuration             0.6
    testPercTempo            0.5
    testDiodeLadder          0.3
    testWaveTable            0.3
    testPads                 0.3
    testKickReference        0.1
    testDynamics             0.1
    testLoudness             0.1
    testTransitions          0.4
    testArrangement          0.9
    testReverb               0.1
    testComposer             0.4
    testPercKit              0.1
    testWav                  0.0
    testMidi                 0.3
    testModelKernel          0.0
    testOscillator           0.0
    testAcid                 0.0
    testMidiKeys             0.0
    testHalfband             0.0
    testParams               0.0
    testTempo                0.0
    testBass                 0.0
    testKick                 0.0
    testLadder               0.0
)

# Placeholder test that fails with a reason: an empty list must not look like a green suite.
function(phos_selftest_broken name reason)
    add_test("selftest.${name}" "${PHOS_CMAKE}" -E echo "${reason}")
    set_tests_properties("selftest.${name}" PROPERTIES WILL_FAIL TRUE LABELS "selftest;quick")
    message(WARNING "${reason}")
endfunction()

if(NOT EXISTS "${PHOS_SELFTEST_EXE}")
    phos_selftest_broken(NOT_BUILT "phos_selftest not built: ${PHOS_SELFTEST_EXE} (build this configuration first)")
    return()
endif()

execute_process(COMMAND "${PHOS_SELFTEST_EXE}" --list
                WORKING_DIRECTORY "${PHOS_SELFTEST_DIR}"
                OUTPUT_VARIABLE _phos_out RESULT_VARIABLE _phos_rc ERROR_VARIABLE _phos_err
                TIMEOUT 60)
string(REPLACE "\r" "" _phos_out "${_phos_out}")
string(REPLACE "\n" ";" _phos_lines "${_phos_out}")
set(_phos_names)
foreach(_l IN LISTS _phos_lines)
    # Names only: finish()'s "0 passed, 0 failed" and blank lines have spaces or nothing.
    if(_l MATCHES "^[A-Za-z_][A-Za-z0-9_.]*$")
        list(APPEND _phos_names "${_l}")
    endif()
endforeach()
if(NOT _phos_rc EQUAL 0 OR NOT _phos_names)
    phos_selftest_broken(LIST_FAILED "phos_selftest --list failed (exit ${_phos_rc}, ${_phos_err}); no sections registered")
    return()
endif()

# The binary's list against the source's run(...) lines: the same table read twice, once compiled and
# once as text. They differ when --list is broken (it would silently drop sections from ctest) or
# when the binary is older than the source -- in both cases ctest would not be testing what the
# source says, and that has to be a red test, not a quiet one.
if(EXISTS "${PHOS_SELFTEST_SOURCE}")
    file(STRINGS "${PHOS_SELFTEST_SOURCE}" _phos_src_lines REGEX "^[ \t]+run\\(\"[A-Za-z0-9_.]+\",")
    set(_phos_src_names)
    foreach(_l IN LISTS _phos_src_lines)
        string(REGEX REPLACE "^[ \t]+run\\(\"([A-Za-z0-9_.]+)\",.*$" "\\1" _l "${_l}")
        list(APPEND _phos_src_names "${_l}")
    endforeach()
    if(NOT "${_phos_src_names}" STREQUAL "${_phos_names}")
        list(LENGTH _phos_src_names _a)
        list(LENGTH _phos_names _b)
        phos_selftest_broken(TABLE_MISMATCH "Tests/selftest.cpp has ${_a} run(...) lines, phos_selftest --list prints ${_b} names (not the same list: rebuild, or --list is broken)")
    endif()
endif()

# The selection itself. An unknown name must fail (a renamed section must not pass as "nothing
# checked"), and --only must take whole names: PHOS_ONLY's rule runs every section whose name occurs
# in the string, so asking for testMidiKeys would run testMidi too. The unknown name is matched on
# its exact outcome -- one failed check and nothing else -- so a crash cannot pass for it.
add_test(selftest.dispatch.unknown "${PHOS_SELFTEST_EXE}" --only testNoSuchSection)
set_tests_properties(selftest.dispatch.unknown PROPERTIES LABELS "selftest;quick"
                     PASS_REGULAR_EXPRESSION "\n0 passed, 1 failed")
if("testMidi" IN_LIST _phos_names AND "testMidiKeys" IN_LIST _phos_names)
    add_test(selftest.dispatch.exact "${PHOS_SELFTEST_EXE}" --only testMidiKeys)
    set_tests_properties(selftest.dispatch.exact PROPERTIES LABELS "selftest;quick"
                         WORKING_DIRECTORY "${PHOS_SELFTEST_DIR}"
                         FAIL_REGULAR_EXPRESSION "== testMidi:")
endif()

# Seconds by name, from the table above.
set(_phos_known)
list(LENGTH PHOS_SELFTEST_SECONDS _n)
if(_n GREATER 1)
    math(EXPR _last "${_n} - 2")
    foreach(_i RANGE 0 ${_last} 2)
        math(EXPR _j "${_i} + 1")
        list(GET PHOS_SELFTEST_SECONDS ${_i} _name)
        list(GET PHOS_SELFTEST_SECONDS ${_j} _secs)
        set(_phos_secs_${_name} ${_secs})
        list(APPEND _phos_known ${_name})
    endforeach()
endif()
foreach(_name IN LISTS _phos_known)
    if(NOT _name IN_LIST _phos_names)
        message(WARNING "Tests/selftest_tests.cmake times ${_name}, which phos_selftest --list no longer has")
    endif()
endforeach()

# Every test gets a temporary directory of its own: testModelFile and testWaveTableLibrary write
# fixed names under std::filesystem::temp_directory_path() (phos_model_test/, phos_broken.phoswt),
# which on Windows is TMP. Two tests at once -- or this suite and another checkout's suite at once --
# would otherwise write and delete each other's files.
function(phos_selftest_env test)
    set(tmp "${PHOS_TEST_TMP}/${test}")
    file(MAKE_DIRECTORY "${tmp}")
    # PHOS_MUTE=1 as for every audible binary started by hand (the self test itself opens no device);
    # TMP/TEMP for the temporary files above; TMPDIR for the same on Linux and macOS; PHOS_PROBE_CACHE so
    # that a probe render any test process has done is not done again (phos/Probe.h -- the sections that
    # test the scheduler and the cache themselves switch it off for their references).
    set_tests_properties("${test}" PROPERTIES
        WORKING_DIRECTORY "${PHOS_SELFTEST_DIR}"
        ENVIRONMENT "PHOS_MUTE=1;TMP=${tmp};TEMP=${tmp};TMPDIR=${tmp};PHOS_PROBE_CACHE=${PHOS_PROBE_CACHE_DIR}"
        # A PHOS_ONLY left in the shell must not shrink the full run (--only ignores it anyway).
        ENVIRONMENT_MODIFICATION "PHOS_ONLY=unset:")
endfunction()

# Sections that render several whole tracks at once on phos::probe::runAll's pool (round
# "test-speed-rest", 20.09.2026; Tests/selftest.cpp has the detail on each). ctest's PROCESSORS is how
# a test tells the scheduler how many of the machine's slots it is really using; left at the default 1,
# ctest would happily start several of these -- each thinking it is cheap -- beside each other and
# beside hosttest/vst3test, oversubscribing the 24 hardware threads well past what any of them was
# measured under. Found the hard way: with hosttest's own PROCESSORS lowered (see Tests/CMakeLists.txt's
# comment on it) and these five left at the ctest default, a `-j 12` run that happened to start four
# presenceArc parts, hosttest, hosttest.realhost and testVariety.levelMatch together pushed hosttest's
# plan time to 31.4 s, over its 30 s bound -- the first FAIL this round saw from contention, not from a
# check. The number here is min(the section's own task count, phos::probe::kMaxThreads=8), i.e. the most
# threads phos::probe::runAll will actually start for it: 8 for testPhaseLock.lock (four tempi x three
# modes, twelve tasks, clamped to 8), 6 for the eight testModalInterchange.presence*/presenceArc* parts
# (kPresencePerSlice), 2 for testVariety.levelMatch (its two independent renders).
set(PHOS_SELFTEST_PROCESSORS
    testPhaseLock.lock                       8
    testModalInterchange.presenceOn1         6
    testModalInterchange.presenceOn2         6
    testModalInterchange.presenceOff1        6
    testModalInterchange.presenceOff2        6
    testModalInterchange.presenceArcOn1      6
    testModalInterchange.presenceArcOn2      6
    testModalInterchange.presenceArcOff1     6
    testModalInterchange.presenceArcOff2     6
    testVariety.levelMatch                   2
)
set(_phos_proc_known)
list(LENGTH PHOS_SELFTEST_PROCESSORS _pn)
if(_pn GREATER 1)
    math(EXPR _plast "${_pn} - 2")
    foreach(_i RANGE 0 ${_plast} 2)
        math(EXPR _j "${_i} + 1")
        list(GET PHOS_SELFTEST_PROCESSORS ${_i} _name)
        list(GET PHOS_SELFTEST_PROCESSORS ${_j} _procs)
        set(_phos_procs_${_name} ${_procs})
        list(APPEND _phos_proc_known ${_name})
    endforeach()
endif()

set(_phos_quick 0)
set(_phos_slow 0)
foreach(_name IN LISTS _phos_names)
    set(_t "selftest.${_name}")
    add_test("${_t}" "${PHOS_SELFTEST_EXE}" --only "${_name}")
    phos_selftest_env("${_t}")
    if(DEFINED _phos_secs_${_name})
        set(_secs ${_phos_secs_${_name}})
    else()
        set(_secs 600)   # unmeasured: start it early and keep it out of `quick`
    endif()
    if(_secs LESS_EQUAL PHOS_QUICK_SECONDS)
        set(_label quick)
        math(EXPR _phos_quick "${_phos_quick} + 1")
    else()
        set(_label slow)
        math(EXPR _phos_slow "${_phos_slow} + 1")
    endif()
    set_tests_properties("${_t}" PROPERTIES LABELS "selftest;${_label}" COST ${_secs})
    if(DEFINED _phos_procs_${_name})
        set_tests_properties("${_t}" PROPERTIES PROCESSORS ${_phos_procs_${_name}})
    endif()
endforeach()
# testWav writes phos_selftest_tmp.wav into the working directory, which every self-test process
# shares (the data files are there); only the full run does the same.
if("testWav" IN_LIST _phos_names)
    set_tests_properties(selftest.testWav PROPERTIES RESOURCE_LOCK phos_selftest_cwd)
endif()

# The old all-in-one run: every section in one process, as before. Kept for whoever wants exactly
# that (and for comparing times), but not part of a plain `ctest`: it would run every section a second
# time. It runs when PHOS_SELFTEST_FULL is set in the environment, e.g.
#     $env:PHOS_SELFTEST_FULL=1; ctest -C Release -L full
# otherwise it is listed as disabled.
add_test(selftest "${PHOS_SELFTEST_EXE}")
phos_selftest_env(selftest)
# COST 0: a disabled test is still "started" in ctest's first pass and holds a slot for that moment;
# with a high cost it did so first and kept the host test (five slots) from starting beside
# testModalInterchange at time zero -- measured, it then waited 340 s.
set_tests_properties(selftest PROPERTIES LABELS "selftest;full" RESOURCE_LOCK phos_selftest_cwd COST 0)
if(NOT "$ENV{PHOS_SELFTEST_FULL}")
    set_tests_properties(selftest PROPERTIES DISABLED TRUE)
endif()
