# Phosphene for Meta Quest -- native OpenXR app (NativeActivity, GLES 3, Oboe), no game engine.
# Configured by build_apk.ps1 with the NDK toolchain; see Quest/README.md.
cmake_minimum_required(VERSION 3.22)
project(PhospheneQuest LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(PHOS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(PHOS_THIRDPARTY "${PHOS_ROOT}/ThirdParty" CACHE PATH "OpenXR loader and Oboe (Quest/fetch_thirdparty.ps1)")

# The same rule as the root CMakeLists: no floating-point contraction. This tree does not include
# that file, so it has to be repeated here -- without it clang fuses a*b+c into an FMA and the scalar
# reference stops matching the NEON lanes, which is exactly what phos_vectest checks on the device.
add_compile_options(-ffp-contract=off)

# The engine, unchanged. PHOS_AVX2 is meaningless on arm64 and Core/CMakeLists.txt already guards it
# on the processor, but it is set to OFF here so the option shows the truth in a cache dump.
set(PHOS_AVX2 OFF CACHE BOOL "" FORCE)
add_subdirectory("${PHOS_ROOT}/Core" Core)

# Khronos OpenXR loader for Android: the prefab layout of the Maven AAR (headers + libopenxr_loader.so).
set(OXR "${PHOS_THIRDPARTY}/openxr-loader/prefab/modules")
if(NOT EXISTS "${OXR}/openxr_loader/libs/android.${ANDROID_ABI}/libopenxr_loader.so")
    message(FATAL_ERROR "OpenXR loader missing under ${PHOS_THIRDPARTY}: run Quest/fetch_thirdparty.ps1")
endif()
add_library(openxr_loader SHARED IMPORTED)
set_target_properties(openxr_loader PROPERTIES
    IMPORTED_LOCATION "${OXR}/openxr_loader/libs/android.${ANDROID_ABI}/libopenxr_loader.so"
    INTERFACE_INCLUDE_DIRECTORIES "${OXR}/headers/include")

# Oboe (low-latency Android audio), built from source as a static library.
add_subdirectory("${PHOS_THIRDPARTY}/oboe" oboe)

# The NDK's native activity glue.
add_library(native_app_glue STATIC "${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c")
target_include_directories(native_app_glue PUBLIC "${ANDROID_NDK}/sources/android/native_app_glue")

add_library(phosquest SHARED src/main.cpp)
target_compile_definitions(phosquest PRIVATE XR_USE_PLATFORM_ANDROID XR_USE_GRAPHICS_API_OPENGL_ES)
target_link_libraries(phosquest PRIVATE PhospheneCore openxr_loader oboe native_app_glue android log EGL GLESv3)
# ANativeActivity_onCreate lives in the glue library and nothing in our code references it.
target_link_options(phosquest PRIVATE -u ANativeActivity_onCreate)
target_compile_options(phosquest PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
