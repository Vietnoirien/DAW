# SPDX-License-Identifier: AGPL-3.0-or-later
# LiBeDAW — Linux Beat DAW
# Copyright (C) 2026 LiBeDAW contributors
# See LICENSE for the full AGPL-3.0 text.

cmake_minimum_required(VERSION 3.21)
project(LiBeDAW VERSION 0.1.0)

include(FetchContent)

# ── JUCE ─────────────────────────────────────────────────────────────────────
FetchContent_Declare(
    juce
    URL https://github.com/juce-framework/JUCE/archive/refs/tags/8.0.12.zip
)
FetchContent_MakeAvailable(juce)

# ── Rubber Band Library (time-stretching / warping) ───────────────────────────
# GPL-2.0-or-later — compatible with LiBeDAW's open-source licence.
# RBL does not ship a CMakeLists.txt; we use the official single-file amalgam
# (single/RubberBandSingle.cpp) to build a static library with plain CMake.
FetchContent_Declare(
    rubberband
    GIT_REPOSITORY https://github.com/breakfastquay/rubberband.git
    GIT_TAG        v3.3.0
    GIT_SHALLOW    TRUE
)
FetchContent_Populate(rubberband)   # downloads only; does NOT call add_subdirectory

add_library(rubberband STATIC
    ${rubberband_SOURCE_DIR}/single/RubberBandSingle.cpp)

target_include_directories(rubberband PUBLIC
    ${rubberband_SOURCE_DIR})          # exposes <rubberband/RubberBandStretcher.h>

target_compile_definitions(rubberband PRIVATE
    NOMINMAX                           # prevent min/max macro pollution on Windows
    _USE_MATH_DEFINES)

# Suppress noisy warnings from the vendored DSP code
target_compile_options(rubberband PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-w>)

# ── clap-juce-extensions (submodule at libs/clap-juce-extensions) ─────────────
# NOTE: This library is included for future CLAP plugin hosting support.
# JUCE 8 natively supports VST3/LV2 hosting only; CLAP host support is
# planned for JUCE 9. The submodule provides the CLAP SDK headers and
# extension infrastructure for when JUCE 9 support lands.
set(CLAP_JUCE_EXTENSIONS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(libs/clap-juce-extensions EXCLUDE_FROM_ALL)

# ── Application target ────────────────────────────────────────────────────────
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
    Source/Instruments/DrumMachineProcessor.cpp
    Source/Instruments/AudioInputProcessor.cpp
    Source/Instruments/PluginInstrumentAdapter.cpp
    Source/Effects/BuiltInEffects.cpp
    Source/Core/AudioClipPlayer.cpp
    Source/Core/CompPlayer.cpp
    Source/ControlSurface/SessionComponent.cpp
    Source/ControlSurface/MixerComponent.cpp
    Source/ControlSurface/ControlSurfaceManager.cpp
    Source/ControlSurface/ApcMiniDriver.cpp)


target_include_directories(LiBeDAW PRIVATE
    Source/Core
    Source/UI
    Source/Sequencing
    Source/Instruments
    Source/Effects
    Source/ControlSurface)

target_compile_features(LiBeDAW PRIVATE cxx_std_20)

target_compile_definitions(LiBeDAW PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_PLUGINHOST_VST3=1)

if (UNIX AND NOT APPLE)
    target_compile_definitions(LiBeDAW PRIVATE JUCE_PLUGINHOST_LV2=1)
endif()

if (APPLE)
    target_compile_definitions(LiBeDAW PRIVATE JUCE_PLUGINHOST_AU=1)
endif()

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
    juce::juce_dsp
    rubberband)

# ── Post-build: create a symlink at LiBeDAW_artefacts/LiBeDAW ─────────────────
# JUCE places the binary under LiBeDAW_artefacts/<Config>/LiBeDAW (e.g. Debug/
# or Release/).  The symlink at the parent level keeps `./build/LiBeDAW_artefacts/LiBeDAW`
# working regardless of the chosen build type, matching what the README documents.
# Symlinks require Unix; skip on Windows where they need elevated privileges.
if (UNIX)
    add_custom_command(TARGET LiBeDAW POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E create_symlink
            $<CONFIG>/LiBeDAW
            ${CMAKE_CURRENT_BINARY_DIR}/LiBeDAW_artefacts/LiBeDAW
        COMMENT "Creating LiBeDAW_artefacts/LiBeDAW symlink → $<CONFIG>/LiBeDAW"
    )
endif()
