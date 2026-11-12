# The ToneGenie audio plugin (JUCE).
#
# Dependencies are downloaded at configure time. To use local checkouts instead:
#   -DFETCHCONTENT_SOURCE_DIR_JUCE=/path/to/JUCE
#   -DFETCHCONTENT_SOURCE_DIR_CLAP_JUCE_EXTENSIONS=/path/to/clap-juce-extensions

include(FetchContent)

set(TONEGENIE_JUCE_TAG "8.0.17-security" CACHE STRING "JUCE version to build against")
set(TONEGENIE_CLAP_EXT_COMMIT "6bde108329ff9fc24404b79b8b397261a75cf6a8" CACHE STRING "clap-juce-extensions commit")

FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG ${TONEGENIE_JUCE_TAG}
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(JUCE)

set(TONEGENIE_FORMATS VST3 Standalone)
if(APPLE)
    list(APPEND TONEGENIE_FORMATS AU)
elseif(UNIX)
    list(APPEND TONEGENIE_FORMATS LV2)
endif()

# AAX (Pro Tools) needs Avid's signing tools to produce a loadable plugin, so it is opt-in.
option(TONEGENIE_BUILD_AAX "Also build an AAX plugin (requires Avid/PACE signing to be usable)" OFF)
if(TONEGENIE_BUILD_AAX AND (APPLE OR WIN32))
    list(APPEND TONEGENIE_FORMATS AAX)
endif()

juce_add_plugin(ToneGenie
    PRODUCT_NAME "ToneGenie"
    COMPANY_NAME "ToneGenie"
    BUNDLE_ID "com.tonegenie.ToneGenie"
    PLUGIN_MANUFACTURER_CODE Tngn
    PLUGIN_CODE Tgn1
    FORMATS ${TONEGENIE_FORMATS}
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD ${TONEGENIE_COPY_AFTER_BUILD}
    VST3_CATEGORIES Fx Distortion
    AU_MAIN_TYPE kAudioUnitType_Effect
    AAX_CATEGORY AAX_ePlugInCategory_Harmonic
    LV2URI "https://github.com/AdiDev29/ToneGenie"
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "ToneGenie processes the guitar connected to your audio input.")

target_sources(ToneGenie PRIVATE
    Source/PluginEditor.cpp
    Source/PluginEditor.h
    Source/PluginProcessor.cpp
    Source/PluginProcessor.h
    Source/Reference.cpp
    Source/Reference.h
    Source/Widgets.cpp
    Source/Widgets.h)

target_compile_definitions(ToneGenie PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_REPORT_APP_USAGE=0
    JUCE_USE_MP3AUDIOFORMAT=1
    JUCE_USE_FLAC=1
    JUCE_USE_OGGVORBIS=1
    JUCE_USE_WINDOWS_MEDIA_FORMAT=1
    JUCE_MODAL_LOOPS_PERMITTED=0
    TONEGENIE_VERSION="${PROJECT_VERSION}")

target_link_libraries(ToneGenie
    PRIVATE
        tonegenie_core
        juce::juce_audio_utils
        juce::juce_audio_formats
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

if(UNIX AND NOT APPLE)
    # Do not depend on the build machine's libstdc++ version.
    target_link_options(ToneGenie PUBLIC -static-libstdc++ -static-libgcc)
endif()

if(TONEGENIE_BUILD_CLAP)
    FetchContent_Declare(clap_juce_extensions
        GIT_REPOSITORY https://github.com/free-audio/clap-juce-extensions.git
        GIT_TAG ${TONEGENIE_CLAP_EXT_COMMIT}
        GIT_SUBMODULES_RECURSE TRUE)
    FetchContent_MakeAvailable(clap_juce_extensions)

    clap_juce_extensions_plugin(TARGET ToneGenie
        CLAP_ID "com.tonegenie.ToneGenie"
        CLAP_FEATURES audio-effect distortion "amp-simulator")
endif()

#===============================================================================
# End-to-end test of the plugin class itself (state, analysis thread, editor).
if(TONEGENIE_BUILD_TESTS)
    add_executable(tonegenie_plugin_test ../tests/PluginTest.cpp)
    target_link_libraries(tonegenie_plugin_test PRIVATE ToneGenie tonegenie_core)
    target_include_directories(tonegenie_plugin_test PRIVATE
        $<TARGET_PROPERTY:ToneGenie,INCLUDE_DIRECTORIES>
        Source
        ../tests)
    target_compile_definitions(tonegenie_plugin_test PRIVATE
        $<TARGET_PROPERTY:ToneGenie,COMPILE_DEFINITIONS>)
    add_test(NAME tonegenie_plugin_test COMMAND tonegenie_plugin_test)
endif()
