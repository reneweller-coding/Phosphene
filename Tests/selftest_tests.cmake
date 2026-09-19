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
#   PHOS_CMAKE          the cmake that configured the tree (for the placeholder tests)
#   PHOS_SELFTEST_SOURCE  Tests/selftest.cpp, whose run(...) lines the list is compared with
#
# The run("name", fn) table in Tests/selftest.cpp's main() is the only list of sections.
# `phos_selftest --list` prints it; every name becomes `selftest.<name>`, running
# `phos_selftest --only <name>`. A section added there is a test here the next time ctest starts,
# with nobody editing CMake -- which was the requirement, because another round adds sections while
# this one is being written.
#
# What stays here is only *metadata*: the measured seconds of each section (below). A section the
# table has and this list does not is labelled `slow` until somebody measures it, so `ctest -L quick`
# never turns into a long run by accident; a name here that the table no longer has is reported.

# Seconds of each section (`== name: s`, each in its own process, from `ctest -C Release -j 6` on
# 19.09.2026, i9-12900K, Release, the machine otherwise ~25 % busy). They become each test's COST, so
# a fresh tree starts the long ones first and the short ones fill the gaps (after one run ctest keeps
# its own costs in Testing/Temporary/CTestCostData.txt); and they decide the label: up to
# PHOS_QUICK_SECONDS is `quick`, above is `slow`. Re-measure when a section moves a lot; nothing
# breaks if a number is stale except the label and the order.
set(PHOS_QUICK_SECONDS 10)
set(PHOS_SELFTEST_SECONDS
    testModalInterchange     620.7
    testVoices               202.9
    testVariety              202.3
    testMixBalance           111.4
    testStereoWidth          100.2
    testPhaseLock            90.0
    testMaster               88.4
    testMelody               81.1
    testAcidVoicing          65.9
    testFoundation           65.8
    testGenreRules           57.9
    testSfxLevel             35.7
    testKickBody             32.7
    testModeColour           30.7
    testEngine               29.9
    testBassRhythm           29.5
    testSectionRules         25.9
    testBandLimit            20.1
    testCues                 13.6
    testAcidColour           7.1
    testMelodyModelWiring    6.3
    testRecipeSpread         5.8
    testPsychedelia          5.5
    testTensionCurve         3.3
    testGateAndDuck          3.0
    testModelDecode          2.4
    testBassBite             1.8
    testArpPatterns          1.7
    testBassModel            1.3
    testForm                 1.2
    testSampler              1.2
    testWaveTableQuality     1.0
    testArrangeDynamics      0.9
    testMotifOperators       0.8
    testSfx                  0.7
    testModelFile            0.7
    testWaveTableLibrary     0.6
    testRhythm               0.5
    testPoly                 0.4
    testCuration             0.4
    testPercTempo            0.4
    testDiodeLadder          0.2
    testWaveTable            0.2
    testPads                 0.1
    testKickReference        0.1
    testDynamics             0.1
    testLoudness             0.1
    testTransitions          0.1
    testReverb               0.1
    testComposer             0.1
    testPercKit              0.1
    testWav                  0.0
    testMidi                 0.0
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
    if(_l MATCHES "^[A-Za-z_][A-Za-z0-9_]*$")
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
    file(STRINGS "${PHOS_SELFTEST_SOURCE}" _phos_src_lines REGEX "^[ \t]+run\\(\"[A-Za-z0-9_]+\",")
    set(_phos_src_names)
    foreach(_l IN LISTS _phos_src_lines)
        string(REGEX REPLACE "^[ \t]+run\\(\"([A-Za-z0-9_]+)\",.*$" "\\1" _l "${_l}")
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
    # TMP/TEMP for the temporary files above; TMPDIR for the same on Linux and macOS.
    set_tests_properties("${test}" PROPERTIES
        WORKING_DIRECTORY "${PHOS_SELFTEST_DIR}"
        ENVIRONMENT "PHOS_MUTE=1;TMP=${tmp};TEMP=${tmp};TMPDIR=${tmp}"
        # A PHOS_ONLY left in the shell must not shrink the full run (--only ignores it anyway).
        ENVIRONMENT_MODIFICATION "PHOS_ONLY=unset:")
endfunction()

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
