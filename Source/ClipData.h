#pragma once
#include <JuceHeader.h>
#include <vector>
#include <cstdint>

// ─── Global DAW Layout Constants ──────────────────────────────────────────────
static constexpr int NUM_SCENES  = 8;
static constexpr int MAX_TRACKS  = 8;

// ─── Track Type ───────────────────────────────────────────────────────────────
enum class TrackType { Audio, Midi };

// ─── Clip Data (UI-thread owned) ─────────────────────────────────────────────
// Plain data struct; no JUCE audio types. Patterns live in PatternPools.
struct ClipData
{
    bool          hasClip        = false;
    juce::String  name;
    juce::Colour  colour         { juce::Colour (0xff2d89ef) };
    bool          isPlaying      = false;

    // Pattern parameters stored for display and relaunching
    int           euclideanSteps  = 16;
    int           euclideanPulses = 4;
    std::vector<uint8_t> hitMap;   // custom per-step override (empty = use generate())
};
