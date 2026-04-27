#pragma once

#include "Pattern.h"
#include <cassert>

struct TrackCommand {
    enum class Type {
        PlayPattern, StopPattern, FlushNotes,
        AuditionNoteOn, AuditionNoteOff,
        MpeExpression   // per-note MPE dimensions (pitch bend, pressure, timbre)
    };

    Type     type;
    Pattern* patternPointer {nullptr};
    double   scheduledSample {-1.0}; // absolute GlobalTransport sample for pattern switch
    int      note     {-1};
    int      velocity {0};

    // ── MPE payload — only valid when type == MpeExpression ─────────────────
    // Zero-initialised for all other command types.
    // Stored as a plain POD so TrackCommand stays trivially copyable
    // (required by LockFreeQueue<TrackCommand, 128>).
    struct MpePayload {
        int   noteId               {-1};   // MIDI note number (0-127)
        int   channel              {0};    // MPE member channel (1-15)
        float pitchBendSemitones   {0.0f}; // ±48 semitones additive pitch offset
        float pressure             {0.0f}; // 0-1 (channel pressure)
        float timbre               {0.5f}; // 0-1 (CC 74 slide; 0.5 = centre)
    };
    MpePayload mpe {};
};

// Render thread copies TrackCommand by value through LockFreeQueue — must stay
// trivially copyable.  This fires at compile time if a field breaks that.
static_assert(std::is_trivially_copyable_v<TrackCommand>,
              "TrackCommand must be trivially copyable for LockFreeQueue");
