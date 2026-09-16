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
