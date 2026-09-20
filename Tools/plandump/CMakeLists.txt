# phos_plandump (round "speed", 20.09.2026): the probe-written fields of N track plans as bit patterns,
# and the time planning them took -- the oracle of "plans bit-identical" and the planning benchmark.
add_executable(phos_plandump main.cpp)
target_link_libraries(phos_plandump PRIVATE PhospheneCore)
if(MSVC)
    target_compile_options(phos_plandump PRIVATE /W4 /permissive- /utf-8)
    target_compile_definitions(phos_plandump PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
