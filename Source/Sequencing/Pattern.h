#pragma once

#include <JuceHeader.h>
#include "GlobalTransport.h"
#include "ClipData.h"

class Pattern {
public:
    virtual ~Pattern() = default;

    std::list<AutomationLane> automationLanes;
    // Read logic used by the audio thread. Must be wait-free and allocate no memory.
    virtual void getEventsForBuffer(juce::MidiBuffer& output, int64_t blockStartSample, int numSamples, const GlobalTransport& transport, double patternStartSample) const = 0;

    // Called by the garbage collector / pool return loop to clear before next use.
    virtual void clear() {
        automationLanes.clear();
    }

    // Used by the UI thread to calculate phase
    virtual double getLengthBeats() const = 0;
};
