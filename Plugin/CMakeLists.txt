# The Phosphene plugin: VST3 and Standalone around the framework-free core.
#
# NEEDS_MIDI_OUTPUT is what makes the VST3 wrapper offer an event output bus; the processor writes
# the score's notes into it (PluginProcessor.cpp, "MIDI out"). MIDI input since 23.09.2026, for
# controllers only: MIDI learn puts a hardware knob on any parameter (phos/MidiMap.h). The generator
# still plays itself; notes coming in are ignored.
juce_add_plugin(Phosphene
    COMPANY_NAME                "Rene Weller"
    BUNDLE_ID                   "com.reneweller.phosphene"
    IS_SYNTH                    TRUE
    # TRUE since 23.09.2026: the plugin reads MIDI controllers for MIDI learn (phos/MidiMap.h); notes are ignored.
    NEEDS_MIDI_INPUT            TRUE
    NEEDS_MIDI_OUTPUT           TRUE
    IS_MIDI_EFFECT              FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    COPY_PLUGIN_AFTER_BUILD     FALSE
    PLUGIN_MANUFACTURER_CODE    Rnwl
    PLUGIN_CODE                 Phos
    FORMATS                     VST3 Standalone
    VST3_CATEGORIES             Instrument Synth
    VERSION                     ${PROJECT_VERSION}
    # The logo's rings (23.09.2026, Tools/logo/make_logo.py --install rings): the standalone's window, taskbar and
    # file icon. The small one is the mark drawn for 16 to 48 pixels, where the full one's 80 points are a speckle.
    ICON_BIG                    "${CMAKE_CURRENT_SOURCE_DIR}/Resources/icon.png"
    ICON_SMALL                  "${CMAKE_CURRENT_SOURCE_DIR}/Resources/icon-small.png"
    PRODUCT_NAME                "Phosphene")

target_sources(Phosphene PRIVATE
    PluginProcessor.cpp
    StoreParameter.cpp
    PlugConductor.cpp
    PluginEditor.cpp
    EditorLayout.cpp
    EditorSetTab.cpp
    EditorArrange.cpp
    EditorPerform.cpp
    EditorGallery.cpp
    EditorHelp.cpp
    EditorFlow.cpp
    EditorMixer.cpp
    EditorScope.cpp
    UpdateCheck.cpp
    PhospheneLookAndFeel.cpp
    Frame.cpp)

# The help page's text (23.09.2026, EditorHelp.cpp): the manual's prose, compiled in, so the help and the PDF
# are one text.
juce_add_binary_data(PhospheneHelpData
    HEADER_NAME PhospheneHelpData.h
    NAMESPACE PhospheneHelpData
    SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/../Tools/manual/chapters.txt
            ${CMAKE_CURRENT_SOURCE_DIR}/Resources/backdrop.jpg ${CMAKE_CURRENT_SOURCE_DIR}/Resources/icon-small.png)

target_compile_definitions(Phosphene PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_REPORT_APP_USAGE=0)

target_link_libraries(Phosphene
    PRIVATE
        PhospheneCore
        PhospheneHelpData
        juce::juce_audio_utils
        juce::juce_osc
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
# 19.09.2026: the voice pack (Vocal.h) beside them; the core finds it through the wavetables' search path.
set(PHOS_RESOURCES library.phoswt melody.phosmdl bass.phosmdl voices.phosvx)
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

# Into bin/<preset> after every build (cmake/Family.cmake, 01.10.2026): the standalone with the files it reads
# beside it, as in its build tree, and the VST3 bundle (its Resources inside).
set(PHOS_BIN_RESOURCES)
foreach(f ${PHOS_RESOURCES})
    list(APPEND PHOS_BIN_RESOURCES "${CMAKE_CURRENT_SOURCE_DIR}/../Core/data/${f}")
endforeach()
family_bin(Phosphene_Standalone FILES ${PHOS_BIN_RESOURCES})
family_bin(Phosphene_VST3 VST3)
