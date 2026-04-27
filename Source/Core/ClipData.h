#pragma once
#include <JuceHeader.h>
#include <vector>
#include <list>
#include <cstdint>

// ─── Global DAW Layout Constants ──────────────────────────────────────────────
// MAX_TRACKS and NUM_SCENES have been removed — both track count and scene
// count are now unbounded at runtime.
// Use numActiveTracks (atomic in MainComponent) and clipGrid[t].size() for
// the live counts.

// ─── Track Type ───────────────────────────────────────────────────────────────
enum class TrackType { Audio, Midi };

// ─── MidiNote (Piano Roll note model) ────────────────────────────────────────
struct MidiNote {
    int    note;         // 0–127 MIDI note number
    double startBeat;    // beat offset from pattern start
    double lengthBeats;  // note duration in beats
    float  velocity;     // 0.0–1.0

    // ── MPE expression snapshot (populated when recorded from an MPE controller) ──
    // hasMpe == false → all expression fields are meaningless and the Piano Roll
    // renders the note in the default blue colour (backward-compatible default).
    bool   hasMpe      = false; // true if any MPE data was recorded for this note
    float  pressure    = 0.0f;  // 0-1; used for Piano Roll pressure colour
    float  pitchBend   = 0.0f;  // semitones additive bend at note-on (snapshot)
    float  timbre      = 0.5f;  // 0-1; CC74 slide value at note-on (0.5 = centre)
};

// ─── Automation Models ────────────────────────────────────────────────────────
struct AutomationPoint {
    double positionBeats; // relative to clip or arrangement start
    float  value;
};

struct AutomationLane {
    juce::String parameterId; // e.g. "Osc A/octave"
    std::vector<AutomationPoint> points;
};

class AutomationRegistry {
public:
    virtual ~AutomationRegistry() = default;
    virtual void registerParameter(const juce::String& id, std::atomic<float>* ptr, float minVal = 0.0f, float maxVal = 1.0f) = 0;
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
    int           euclideanBars   = 1;   // 1, 2, or 4 bars duration
    std::vector<uint8_t> hitMap;   // custom per-step override (empty = use generate())

    // Drum Rack per-pad euclidean patterns
    struct PadPattern {
        int steps = 16;
        int pulses = 0;
        int bars   = 1;   // 1, 2, or 4 bars
        std::vector<uint8_t> hitMap;
    };
    PadPattern drumPatterns[16];

    // Piano-roll notes (used when patternMode == "pianoroll" or generated from drumPatterns)
    std::vector<MidiNote> midiNotes;
    double        patternLengthBars = 1.0; // 1, 2, or 4
    juce::String  patternMode    = "euclidean"; // "euclidean" | "pianoroll" | "drumrack"
    
    // Clip-level parameter automation
    std::list<AutomationLane> automationLanes;
};

// ─── Warp Marker (3.1) ────────────────────────────────────────────────────────
// A beat-anchor that pins a position in the raw audio file (source) to a
// specific position on the project timeline (target).  A sorted list of these
// inside ArrangementClip drives the per-segment stretch ratio calculation.
struct WarpMarker {
    double sourcePositionSeconds = 0.0; // absolute time inside the raw audio file
    double targetBeat            = 0.0; // 0-based beat on the project timeline
                                         // (relative to clip start in beats)
};

// ─── Take (3.2) ───────────────────────────────────────────────────────────────
// One recorded pass of audio.  All takes inside an ArrangementClip share the
// same timeline position as the clip itself.
//
// LIMIT: A maximum of kMaxTakesPerClip (8) takes is supported per clip.
// Each take requires one decoder thread (AudioClipPlayer::DecoderThread).
// Exceeding 8 takes causes the oldest take to be dropped at record time.
struct Take {
    static constexpr int kMaxTakesPerClip = 8;

    juce::String audioFilePath;          // absolute path to recorded WAV on disk
    double       recordedStartSec = 0.0; // transport position (seconds) of sample 0
    double       lengthSec        = 0.0; // duration of the raw audio file
    int          takeIndex        = 0;   // 0-based slot (determines player index)
};

// ─── CompRegion (3.2) ────────────────────────────────────────────────────────
// A contiguous time slice inside an ArrangementClip where a specific take is
// active.  CompRegions must be sorted by startBeat and non-overlapping.
// An empty compRegions vector means take[0] plays for the full clip extent.
struct CompRegion {
    int    takeIndex      = 0;    // index into ArrangementClip::takes (0-based)
    double startBeat      = 0.0;  // clip-relative beat offset (0 = clip start)
    double endBeat        = 0.0;  // clip-relative beat offset (exclusive)
    // Crossfade half-lengths in samples, determined by background zero-crossing
    // search at comp-edit time.  Defaults to ~10 ms @ 48 kHz.
    int    fadeInSamples  = 480;
    int    fadeOutSamples = 480;
};

// ─── Arrangement Placement ────────────────────────────────────────────────
struct ArrangementClip {
    int         trackIndex   = -1;
    int         sourceScene  = -1;   // which clipGrid row this came from (-1 = custom)
    double      startBar     = 0.0;  // 1-based bar position on the timeline
    double      lengthBars   = 1.0;  // duration in bars
    ClipData    data;                // full clip data (name, colour, notes, etc.)
    
    // Arrangement-level parameter automation (overrides clip automation if present)
    std::list<AutomationLane> automationLanes;

    // ── Warp / time-stretch data (3.1) ────────────────────────────────────────
    // audioFilePath: absolute path to a WAV/FLAC/AIFF on disk.  Empty = MIDI clip.
    juce::String audioFilePath;

    // clipBpm: the original tempo of the audio material.  0.0 = unknown (manual).
    // When > 0, the render thread derives the stretch ratio from projectBPM / clipBpm.
    double clipBpm = 0.0;

    // warpEnabled: if false the clip is played at native speed (no stretching).
    bool warpEnabled = false;

    // WarpMode: which DSP algorithm to use.  Complex = Phase Vocoder (R3 engine).
    // Beats and Tones are UI-stubbed; they map to future RBL option flags (v0.2).
    enum class WarpMode { Complex, Beats, Tones };
    WarpMode warpMode = WarpMode::Complex;

    // warpMarkers: sorted list of beat-anchor pairs.  When non-empty, the stretch
    // ratio is computed segment-by-segment between consecutive markers.
    std::vector<WarpMarker> warpMarkers;

    // ── Comping (3.2) ─────────────────────────────────────────────────────────
    // takes: all recorded passes for this clip position.  Empty = legacy
    //   single-file mode (audioFilePath + warpEnabled path is used instead).
    // compRegions: sorted, non-overlapping active-take time slices.
    //   Empty compRegions + non-empty takes → take[0] plays for the full clip.
    //
    // NOTE: warpEnabled and non-empty takes are mutually exclusive in v0.1.
    //   When takes.size() > 0, the CompPlayer is used regardless of warpEnabled.
    //   Full warp-inside-comp support is deferred to v0.2 (see ideas_for_v0.2.md).
    std::vector<Take>       takes;
    std::vector<CompRegion> compRegions;
};

// ─── Thread-Safe Arrangement Timeline ─────────────────────────────────────
struct SharedArrangement {
    // Dynamic: one inner vector per active track (size == numActiveTracks at snapshot time).
    std::vector<std::vector<ArrangementClip>> tracks;
};
