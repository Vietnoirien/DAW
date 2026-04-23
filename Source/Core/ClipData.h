#pragma once
#include <JuceHeader.h>
#include <vector>
#include <cstdint>

// ─── Global DAW Layout Constants ──────────────────────────────────────────────
static constexpr int NUM_SCENES  = 8;
// MAX_TRACKS has been removed — track count is now unbounded at runtime.
// Use numActiveTracks (atomic in MainComponent) as the live count.

// ─── Track Type ───────────────────────────────────────────────────────────────
enum class TrackType { Audio, Midi };

// ─── MidiNote (Piano Roll note model) ────────────────────────────────────────
struct MidiNote {
    int    note;         // 0–127 MIDI note number
    double startBeat;    // beat offset from pattern start
    double lengthBeats;  // note duration in beats
    float  velocity;     // 0.0–1.0
};

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

    // Drum Rack per-pad euclidean patterns
    struct PadPattern {
        int steps = 16;
        int pulses = 0;
        std::vector<uint8_t> hitMap;
    };
    PadPattern drumPatterns[16];

    // Piano-roll notes (used when patternMode == "pianoroll" or generated from drumPatterns)
    std::vector<MidiNote> midiNotes;
    double        patternLengthBars = 1.0; // 1, 2, or 4
    juce::String  patternMode    = "euclidean"; // "euclidean" | "pianoroll" | "drumrack"
};

// ─── Arrangement Placement ────────────────────────────────────────────────
struct ArrangementClip {
    int         trackIndex   = -1;
    int         sourceScene  = -1;   // which clipGrid row this came from (-1 = custom)
    double      startBar     = 0.0;  // 1-based bar position on the timeline
    double      lengthBars   = 1.0;  // duration in bars
    ClipData    data;                // full clip data (name, colour, notes, etc.)
};

// ─── Thread-Safe Arrangement Timeline ─────────────────────────────────────
struct SharedArrangement {
    // Dynamic: one inner vector per active track (size == numActiveTracks at snapshot time).
    std::vector<std::vector<ArrangementClip>> tracks;
};
