add_executable(phos_render main.cpp)
target_link_libraries(phos_render PRIVATE PhospheneCore)
if(MSVC)
    target_compile_options(phos_render PRIVATE /W4 /permissive- /utf-8)
    target_compile_definitions(phos_render PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# Into bin/<preset> after every build (cmake/Family.cmake, 01.10.2026).
family_bin(phos_render)
