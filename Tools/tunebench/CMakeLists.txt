# phos_tunebench (10.11.2026): every factory preset of a synth playing one note, for Tools/tuning_audit.py -- where
# the energy of a sound lies against the pitch of its note.
add_executable(phos_tunebench main.cpp)
target_link_libraries(phos_tunebench PRIVATE PhospheneCore)
if(MSVC)
    target_compile_options(phos_tunebench PRIVATE /W4 /permissive- /utf-8)
    target_compile_definitions(phos_tunebench PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
