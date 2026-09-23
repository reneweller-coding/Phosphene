# The plan snapshot (23.09.2026, round "Schnappschuss"): runs `phos_plandump --decisions` with the arguments
# the golden file names in its first line and compares the output with the rest of that file.
#
#   cmake -DPLANDUMP=<phos_plandump> -DGOLDEN=<Tests/golden/x.txt> -P Tests/plan_snapshot.cmake
#   cmake -DPLANDUMP=<phos_plandump> -DGOLDEN=<Tests/golden/x.txt> -DUPDATE=1 -P Tests/plan_snapshot.cmake
#
# The first line of a golden file is `# args: <phos_plandump arguments>`; everything after it is the dump.
# A mismatch prints the first differing lines with their numbers, which is where a review starts: every
# decision the composer makes is a line of the dump, so a change that was not meant shows up as the lines it
# moved. When the change *was* meant, UPDATE=1 (or Tools/update_snapshots.py for all of them) writes the new
# dump and the diff goes into the commit, where it is the record of what the commit changed musically.
cmake_minimum_required(VERSION 3.20)
if(NOT PLANDUMP OR NOT GOLDEN)
    message(FATAL_ERROR "plan_snapshot.cmake needs -DPLANDUMP=... and -DGOLDEN=...")
endif()
file(STRINGS "${GOLDEN}" first LIMIT_COUNT 1)
if(NOT first MATCHES "^# args: (.*)$")
    message(FATAL_ERROR "${GOLDEN}: the first line must be '# args: ...'")
endif()
set(argline "${CMAKE_MATCH_1}")
separate_arguments(args UNIX_COMMAND "${argline}")
execute_process(COMMAND "${PLANDUMP}" --decisions ${args}
                OUTPUT_VARIABLE out ERROR_VARIABLE err RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "phos_plandump failed (${rc}): ${err}")
endif()
string(REPLACE "\r" "" out "${out}")
if(UPDATE)
    file(WRITE "${GOLDEN}" "# args: ${argline}\n${out}")
    message(STATUS "updated ${GOLDEN}")
    return()
endif()
file(READ "${GOLDEN}" golden)
string(REPLACE "\r" "" golden "${golden}")
# Not REGEX REPLACE "^[^\n]*\n": CMake's REPLACE applies the anchor again after every match and strips every line.
string(FIND "${golden}" "\n" cut)
math(EXPR cut "${cut} + 1")
string(SUBSTRING "${golden}" ${cut} -1 golden)
if(golden STREQUAL out)
    message(STATUS "plan snapshot ${GOLDEN}: identical")
    return()
endif()
# The first differing lines, numbered as in the golden file (line 1 is the args line).
string(REPLACE ";" "," golden "${golden}")
string(REPLACE ";" "," out "${out}")
string(REPLACE "\n" ";" gl "${golden}")
string(REPLACE "\n" ";" ol "${out}")
list(LENGTH gl ng)
list(LENGTH ol no)
set(n ${ng})
if(no GREATER n)
    set(n ${no})
endif()
set(shown 0)
set(report "")
math(EXPR last "${n} - 1")
foreach(i RANGE 0 ${last})
    set(g "")
    set(o "")
    if(i LESS ng)
        list(GET gl ${i} g)
    endif()
    if(i LESS no)
        list(GET ol ${i} o)
    endif()
    if(NOT g STREQUAL o)
        math(EXPR line "${i} + 2")
        string(APPEND report "  line ${line}\n    golden: ${g}\n    now:    ${o}\n")
        math(EXPR shown "${shown} + 1")
        if(shown GREATER_EQUAL 12)
            break()
        endif()
    endif()
endforeach()
message(FATAL_ERROR "plan snapshot ${GOLDEN} differs (golden ${ng} lines, now ${no}); first differences:\n${report}"
                    "If the change is meant: python Tools/update_snapshots.py --plandump ${PLANDUMP}")
