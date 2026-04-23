cmake_minimum_required(VERSION 3.20)
project(LiBeDAW VERSION 0.1.0)

include(FetchContent)

# Fetch JUCE via CMake
FetchContent_Declare(
    juce
    URL https://github.com/juce-framework/JUCE/archive/refs/tags/8.0.0.zip
)
FetchContent_MakeAvailable(juce)

# Create the executable
juce_add_gui_app(LiBeDAW
    PRODUCT_NAME "LiBeDAW"
    VERSION ${PROJECT_VERSION})

juce_generate_juce_header(LiBeDAW)

target_sources(LiBeDAW PRIVATE
    Source/Core/Main.cpp
    Source/Core/MainComponent.cpp
    Source/Core/MainComponent.h
    Source/Instruments/OscProcessor.cpp
    Source/Instruments/SimplerProcessor.cpp
    Source/Instruments/DrumRackProcessor.cpp
    Source/Instruments/FMSynthProcessor.cpp
    Source/Instruments/WavetableSynthProcessor.cpp
    Source/Instruments/KarplusStrongProcessor.cpp
    Source/Effects/BuiltInEffects.cpp)

target_include_directories(LiBeDAW PRIVATE
    Source/Core
    Source/UI
    Source/Sequencing
    Source/Instruments
    Source/Effects)

target_compile_features(LiBeDAW PRIVATE cxx_std_20)

target_compile_definitions(LiBeDAW PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_LV2=1)

target_link_libraries(LiBeDAW PRIVATE
    juce::juce_core
    juce::juce_data_structures
    juce::juce_events
    juce::juce_graphics
    juce::juce_gui_basics
    juce::juce_gui_extra
    juce::juce_audio_basics
    juce::juce_audio_devices
    juce::juce_audio_formats
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_dsp)
