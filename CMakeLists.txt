cmake_minimum_required(VERSION 3.20)
project(LinuxDAW VERSION 0.1.0)

include(FetchContent)

# Fetch JUCE via CMake
FetchContent_Declare(
    juce
    URL https://github.com/juce-framework/JUCE/archive/refs/tags/8.0.0.zip
)
FetchContent_MakeAvailable(juce)

# Create the executable
juce_add_gui_app(LinuxDAW
    PRODUCT_NAME "LinuxDAW"
    VERSION ${PROJECT_VERSION})

juce_generate_juce_header(LinuxDAW)

target_sources(LinuxDAW PRIVATE
    Source/Main.cpp
    Source/MainComponent.cpp
    Source/MainComponent.h)

target_compile_features(LinuxDAW PRIVATE cxx_std_20)

target_compile_definitions(LinuxDAW PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_LV2=1)

target_link_libraries(LinuxDAW PRIVATE
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
    juce::juce_audio_utils)
