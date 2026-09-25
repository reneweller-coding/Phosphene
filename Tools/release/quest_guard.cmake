# Phosphene -- the build guard: the two ways this repository can break without any test noticing.
#
# Run by ctest as the test "questguard" (Tests/CMakeLists.txt), and by Tools/release/build_matrix.ps1
# before the long matrix, so a five-second answer comes before a forty-minute one:
#
#   cmake -DPHOS_ROOT=<repo> -DPHOS_SCRATCH=<dir> [-DPHOS_ANDROID_NDK=<ndk>] -P quest_guard.cmake
#
# WHY THIS EXISTS
#
# Two defects were found on 16.09.2026 that no test could have caught, because nothing in the
# repository builds the artefacts they live in:
#
#   1. Quest/src/main.cpp had not compiled since Phase 5. It still used kMelodyMaxBlocks and
#      MelodyPlan::blockParts/padGate, all of which the form grammar removed. ctest never touches
#      that file -- the Quest app is its own CMake project, built by Quest/build_apk.ps1 -- so the
#      break sat there for a phase and a half and surfaced only when somebody built an APK by hand.
#   2. Configuring the root project for Android failed, because PHOS_BUILD_PLUGIN defaulted ON and
#      JUCE cannot cross-compile the host-side helper (juceaide) it needs to make a plugin. The
#      error read "No CMAKE_C_COMPILER could be found", which sends the reader into the NDK
#      toolchain rather than at the option that caused it. Fixed in master f0733e0 by defaulting the
#      option off on Android -- and nothing would have noticed if that default came back.
#
# WHAT IT DOES, AND WHAT IT DOES NOT
#
#   * Configures the root project for Android with the DEFAULTS. Passing -DPHOS_BUILD_PLUGIN=OFF
#     here would test the workaround instead of the default, which is the thing that regressed.
#   * Asserts PHOS_BUILD_PLUGIN=OFF in the resulting cache, so the failure message names the option
#     rather than leaving the reader with JUCE's.
#   * Compiles every Quest translation unit with the NDK's own clang++, syntax only (-fsyntax-only:
#     no code generation, no objects, no linking). That is the compiler that matters, with the real
#     headers, so it neither misses a break nor invents one.
#
# It does NOT link. A missing symbol in libphosquest.so is caught by build_matrix.ps1's Android
# entry, which is the slow guard; this one is the cheap one that runs every time.
#
# WHY NOT STUB HEADERS. The alternative -- fake <android/*.h>, <openxr/openxr.h>, <oboe/Oboe.h>,
# <EGL/egl.h>, <GLES3/gl3.h>, <jni.h> and the native-activity glue, so MSVC could parse the file
# with /Zs and no NDK at all -- was rejected after reading the includes: OpenXR alone is a few
# thousand declarations, and every new call in main.cpp would need a new stub. A guard that has to
# be repaired whenever the guarded code changes gets switched off. Measured cost of the real
# compiler instead: 1.6 s.
#
# WITHOUT THE NDK the guard cannot run. It then exits 77, which Tests/CMakeLists.txt declares as
# ctest's SKIP_RETURN_CODE: the suite shows "Skipped" with the reason, which is honest, rather than
# red (which people learn to ignore) or green (which is a lie).

cmake_minimum_required(VERSION 3.22)

if(NOT DEFINED PHOS_ROOT)
    message(FATAL_ERROR "quest_guard.cmake needs -DPHOS_ROOT=<repository root>")
endif()
if(NOT DEFINED PHOS_SCRATCH)
    set(PHOS_SCRATCH "${PHOS_ROOT}/build-guard")
endif()

set(_skip 77)   # ctest's SKIP_RETURN_CODE, see Tests/CMakeLists.txt

# ---------------------------------------------------------------- find the NDK
# In order: what the caller passed, what the usual environment variables say, then the layout this
# machine has (C:/Android-Buildtools/sdk/ndk/<version>, newest first -- the toolchain note in
# docs/rounds/2026-09.md names r27 at 27.2.12479018).
if(NOT DEFINED PHOS_ANDROID_NDK OR PHOS_ANDROID_NDK STREQUAL "")
    foreach(_var ANDROID_NDK_HOME ANDROID_NDK_ROOT ANDROID_NDK)
        if(DEFINED ENV{${_var}} AND EXISTS "$ENV{${_var}}/build/cmake/android.toolchain.cmake")
            set(PHOS_ANDROID_NDK "$ENV{${_var}}")
            break()
        endif()
    endforeach()
endif()
if(NOT DEFINED PHOS_ANDROID_NDK OR PHOS_ANDROID_NDK STREQUAL "")
    file(GLOB _ndks LIST_DIRECTORIES true "C:/Android-Buildtools/sdk/ndk/*")
    list(SORT _ndks)
    list(REVERSE _ndks)
    foreach(_n IN LISTS _ndks)
        if(EXISTS "${_n}/build/cmake/android.toolchain.cmake")
            set(PHOS_ANDROID_NDK "${_n}")
            break()
        endif()
    endforeach()
endif()
if(NOT DEFINED PHOS_ANDROID_NDK OR NOT EXISTS "${PHOS_ANDROID_NDK}/build/cmake/android.toolchain.cmake")
    message("questguard: SKIPPED -- no Android NDK found.")
    # The variable names, not their values: CMake would expand an unset one to nothing and the line
    # would read "Looked at //, and ...", which tells the reader neither what was searched nor why.
    message("  Looked at -DPHOS_ANDROID_NDK, the environment variables ANDROID_NDK_HOME,")
    message("  ANDROID_NDK_ROOT and ANDROID_NDK, and C:/Android-Buildtools/sdk/ndk/*.")
    message("  Install NDK r27, or point one of those at a checkout that has")
    message("  build/cmake/android.toolchain.cmake in it.")
    message("  Unchecked while skipped: that Quest/src/*.cpp still compiles, and that the root")
    message("  project still configures for Android with its default options.")
    cmake_language(EXIT ${_skip})
endif()

# No ninja ships with this SDK, so the generator is Unix Makefiles driven by the NDK's own make.
set(_make "${PHOS_ANDROID_NDK}/prebuilt/windows-x86_64/bin/make.exe")
if(NOT EXISTS "${_make}")
    set(_make "${PHOS_ANDROID_NDK}/prebuilt/linux-x86_64/bin/make")
endif()
set(_clang "${PHOS_ANDROID_NDK}/toolchains/llvm/prebuilt/windows-x86_64/bin/clang++.exe")
if(NOT EXISTS "${_clang}")
    set(_clang "${PHOS_ANDROID_NDK}/toolchains/llvm/prebuilt/linux-x86_64/bin/clang++")
endif()

# ---------------------------------------------------------------- the third-party headers
# OpenXR and Oboe are not in the repository (ThirdParty/ is gitignored); Quest/fetch_thirdparty.ps1
# puts them there, as junctions into the sibling Noctuary checkout when that is present. Without
# them the Quest sources cannot even be parsed, so this is a skip and not a failure -- but it names
# the script, because the next thing the reader needs is the command.
set(_oxr "${PHOS_ROOT}/ThirdParty/openxr-loader/prefab/modules/headers/include")
set(_oboe "${PHOS_ROOT}/ThirdParty/oboe/include")
if(NOT EXISTS "${_oxr}/openxr/openxr.h" OR NOT EXISTS "${_oboe}/oboe/Oboe.h")
    message("questguard: SKIPPED -- the Quest headers are not here.")
    message("  Missing ${_oxr}/openxr/openxr.h or ${_oboe}/oboe/Oboe.h.")
    message("  Run: powershell -File ${PHOS_ROOT}/Quest/fetch_thirdparty.ps1")
    message("  Unchecked while skipped: that Quest/src/*.cpp still compiles.")
    cmake_language(EXIT ${_skip})
endif()

message("questguard: NDK ${PHOS_ANDROID_NDK}")

# ---------------------------------------------------------------- 1. the Android configure
# Deliberately no -DPHOS_BUILD_PLUGIN: the default is what broke, so the default is what is tested.
# A fresh directory every time, because a cached CMAKE_C_COMPILER would hide exactly this failure.
set(_cfg "${PHOS_SCRATCH}/android-default")
file(REMOVE_RECURSE "${_cfg}")
file(MAKE_DIRECTORY "${_cfg}")
message("questguard: configuring the root project for Android (arm64-v8a, android-29), default options")
execute_process(
    COMMAND ${CMAKE_COMMAND} -S "${PHOS_ROOT}" -B "${_cfg}" -G "Unix Makefiles"
            "-DCMAKE_MAKE_PROGRAM=${_make}"
            "-DCMAKE_TOOLCHAIN_FILE=${PHOS_ANDROID_NDK}/build/cmake/android.toolchain.cmake"
            -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DCMAKE_BUILD_TYPE=Release
            -Wno-deprecated
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0)
    message("${_out}")
    message("${_err}")
    message(FATAL_ERROR
        "questguard: the root project no longer configures for Android with its default options.\n"
        "  If the error mentions CMAKE_C_COMPILER, read it as JUCE and not as the NDK: JUCE builds a\n"
        "  host-side helper (juceaide) to make a plugin, and a cross-compiler cannot produce one.\n"
        "  PHOS_BUILD_PLUGIN has to default OFF when ANDROID is set (root CMakeLists.txt).")
endif()

# Belt and braces: even if some future change let the configure succeed for another reason, the
# option itself is the contract. Reading it back also puts the true value in the log.
file(STRINGS "${_cfg}/CMakeCache.txt" _plugin_line REGEX "^PHOS_BUILD_PLUGIN:BOOL=")
if(NOT _plugin_line MATCHES "=(OFF|0|FALSE|NO|N)$")
    message(FATAL_ERROR
        "questguard: the Android cache says ${_plugin_line}.\n"
        "  The JUCE plugin is a desktop artefact and must not be on by default in an Android\n"
        "  configure (root CMakeLists.txt). Quest/build_apk.ps1 does not build the root project at\n"
        "  all, so nothing else would have noticed.")
endif()
message("questguard: Android configure ok, ${_plugin_line}")

# ---------------------------------------------------------------- 2. the Quest translation units
# Every .cpp under Quest/src, not just main.cpp, so a file added later is guarded the day it lands.
# The flags mirror Quest/CMakeLists.txt: the same standard, the same XR defines, the same warning
# set. -ffp-contract=off is carried over too -- it is a correctness flag for this project (the NEON
# lanes are compared bit for bit against the scalar path), and a guard compiled under other rules
# would be a guard of something else.
file(GLOB _quest_src "${PHOS_ROOT}/Quest/src/*.cpp")
if(_quest_src STREQUAL "")
    message(FATAL_ERROR "questguard: no sources under ${PHOS_ROOT}/Quest/src")
endif()
set(_glue "${PHOS_ANDROID_NDK}/sources/android/native_app_glue")
foreach(_src IN LISTS _quest_src)
    get_filename_component(_name "${_src}" NAME)
    execute_process(
        COMMAND "${_clang}" --target=aarch64-linux-android29 -std=c++20 -fsyntax-only
                -ffp-contract=off -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
                -DXR_USE_PLATFORM_ANDROID -DXR_USE_GRAPHICS_API_OPENGL_ES
                "-I${PHOS_ROOT}/Core/include" "-I${_oxr}" "-I${_oboe}" "-I${_glue}"
                "${_src}"
        RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    if(NOT _rc EQUAL 0)
        message("${_out}")
        message("${_err}")
        message(FATAL_ERROR
            "questguard: Quest/src/${_name} does not compile for the headset.\n"
            "  Nothing else in ctest builds it -- the Quest app is its own CMake project, made by\n"
            "  Quest/build_apk.ps1 -- so this is the only place the break shows. The usual cause is\n"
            "  a change in Core/ that the Quest app still calls the old way.")
    endif()
    message("questguard: ${_name} compiles for aarch64-linux-android29")
endforeach()

message("questguard: passed")
