#pragma once

#include <JuceHeader.h>
#include "Pattern.h"
#include <vector>

struct MidiEventDef {
    juce::MidiMessage message;
    double startBeat;
};

class MidiPattern : public Pattern {
public:
    MidiPattern() {
        events.reserve(1000); // Pre-allocate capacity to avoid allocation during fill
    }

    void addEvent(const juce::MidiMessage& msg, double startBeat) {
        events.push_back({msg, startBeat});
    }

    void getEventsForBuffer(juce::MidiBuffer& output, int64_t blockStartSample, int numSamples, const GlobalTransport& transport, double patternStartSample) const override {
        double spb = transport.getSamplesPerBeat();
        if (spb <= 0.0) return;

        // Note: For simplicity in prototype, assuming the pattern loops every 4 beats.
        const double patternLengthBeats = 4.0;
        double patternLengthSamples = patternLengthBeats * spb;

        if (patternLengthSamples <= 0.0) return;

        if (static_cast<double>(blockStartSample + numSamples) <= patternStartSample) return;

        double blockStart = static_cast<double>(blockStartSample) - patternStartSample;
        double blockEnd = blockStart + numSamples;

        if (blockStart < 0.0) {
            blockStart = 0.0;
        }

        for (const auto& ev : events) {
            double evOffsetSamples = ev.startBeat * spb;

            long kStart = static_cast<long>(std::ceil((blockStart - evOffsetSamples) / patternLengthSamples)) - 1;
            long kEnd = static_cast<long>(std::ceil((blockEnd - evOffsetSamples) / patternLengthSamples)) + 1;

            for (long k = kStart; k <= kEnd; ++k) {
                double exactStart = k * patternLengthSamples + evOffsetSamples;
                int64_t globalSample = static_cast<int64_t>(std::round(exactStart + patternStartSample));
                
                if (globalSample >= blockStartSample && globalSample < blockStartSample + numSamples) {
                    int bufferSampleOffset = static_cast<int>(globalSample - blockStartSample);
                    output.addEvent(ev.message, bufferSampleOffset);
                }
            }
        }
    }

    void clear() override {
        events.clear(); // Does not shrink capacity (vector::clear keeps allocation)
    }

    double getLengthBeats() const override {
        return 4.0; // Currently hardcoded to 4 beats
    }

private:
    std::vector<MidiEventDef> events;
};
