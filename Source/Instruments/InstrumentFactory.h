#pragma once
#include <JuceHeader.h>
#include <memory>
#include "Instrument.h"

// We include the concrete implementations here to instantiate them
#include "OscProcessor.h"
#include "DrumRackProcessor.h"
#include "SimplerProcessor.h"

class InstrumentFactory {
public:
    static std::unique_ptr<InstrumentProcessor> create(const juce::String& name) {
        if (name == "Oscillator") return std::make_unique<OscProcessor>();
        if (name == "Simpler")    return std::make_unique<SimplerProcessor>();
        if (name == "DrumRack")   return std::make_unique<DrumRackProcessor>();
        return nullptr;
    }

    static juce::StringArray getAvailableInstruments() {
        return { "Oscillator", "Simpler", "DrumRack" };
    }
};
