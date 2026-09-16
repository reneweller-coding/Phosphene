# The Phosphene plugin: VST3 and Standalone around the framework-free core.
#
# NEEDS_MIDI_OUTPUT is what makes the VST3 wrapper offer an event output bus; the processor writes
# the score's notes into it (PluginProcessor.cpp, "MIDI out"). No MIDI input: the generator plays
# itself, the host only supplies transport and tempo.
juce_add_plugin(Phosphene
    COMPANY_NAME                "Rene Weller"
    BUNDLE_ID                   "com.reneweller.phosphene"
    IS_SYNTH                    TRUE
    NEEDS_MIDI_INPUT            FALSE
    NEEDS_MIDI_OUTPUT           TRUE
    IS_MIDI_EFFECT              FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    COPY_PLUGIN_AFTER_BUILD     FALSE
    PLUGIN_MANUFACTURER_CODE    Rnwl
    PLUGIN_CODE                 Phos
    FORMATS                     VST3 Standalone
    VST3_CATEGORIES             Instrument Synth
    VERSION                     ${PROJECT_VERSION}
    PRODUCT_NAME                "Phosphene")

target_sources(Phosphene PRIVATE
    PluginProcessor.cpp
    PluginEditor.cpp
    EditorLayout.cpp
    EditorSetTab.cpp
    EditorArrange.cpp
    EditorPerform.cpp
    PhospheneLookAndFeel.cpp)

target_compile_definitions(Phosphene PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_REPORT_APP_USAGE=0)

target_link_libraries(Phosphene
    PRIVATE
        PhospheneCore
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

# The shipped resources, installed beside the artefacts (PluginProcessor.cpp,
# resolveResourceDirectory()): the wavetable pack and the two learned models of Phase 8. Without
# these copies the plugin has nothing to find -- PHOS_SOURCE_DATA_DIR only ever points at a source
# tree and a shipping build does not define it at all (Core/CMakeLists.txt) -- so an installed
# plugin would fall back to the six built-in tables and to the Markov composer, and never say why.
#
# The standalone gets them beside its executable; the VST3 gets them in Contents/Resources, which is
# where the VST3 specification keeps a plugin's data and the one place a host copying a bundle takes
# with it. The release staging packages exactly these two locations (Deploy/build_release.ps1) and
# the package check proves both are filled (Tools/release/check_package.ps1).
set(PHOS_RESOURCES library.phoswt melody.phosmdl bass.phosmdl)
foreach(f ${PHOS_RESOURCES})
    if(TARGET Phosphene_Standalone)
        add_custom_command(TARGET Phosphene_Standalone POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${CMAKE_CURRENT_SOURCE_DIR}/../Core/data/${f}" "$<TARGET_FILE_DIR:Phosphene_Standalone>/${f}"
            VERBATIM)
    endif()
    if(TARGET Phosphene_VST3)
        add_custom_command(TARGET Phosphene_VST3 POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:Phosphene_VST3>/../Resources"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${CMAKE_CURRENT_SOURCE_DIR}/../Core/data/${f}" "$<TARGET_FILE_DIR:Phosphene_VST3>/../Resources/${f}"
            VERBATIM)
    endif()
endforeach()

# The composer's probe renders put whole Engine objects on the stack (see Composer.cpp); the
# standalone's message thread gets the same 16 MB the offline tools get.
if(MSVC)
    target_compile_options(Phosphene PRIVATE /utf-8)
    foreach(t Phosphene_Standalone Phosphene_VST3)
        if(TARGET ${t})
            target_link_options(${t} PRIVATE "LINKER:/STACK:16777216")
        endif()
    endforeach()
endif()
