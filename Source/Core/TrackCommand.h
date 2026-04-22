#pragma once

#include "Pattern.h"

struct TrackCommand {
    enum class Type { PlayPattern, StopPattern, FlushNotes };
    Type type;
    Pattern* patternPointer {nullptr};
    double scheduledSample {-1.0}; // The absolute GlobalTransport sample when the pattern should become active
};
