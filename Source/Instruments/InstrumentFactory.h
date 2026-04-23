#pragma once
#include <JuceHeader.h>
#include <memory>
#include "Instrument.h"

// We include the concrete implementations here to instantiate them
#include "OscProcessor.h"
#include "DrumRackProcessor.h"
#include "SimplerProcessor.h"
#include "FMSynthProcessor.h"
#include "WavetableSynthProcessor.h"
#include "KarplusStrongProcessor.h"

class InstrumentFactory {
public:
    static std::unique_ptr<InstrumentProcessor> create(const juce::String& name) {
        if (name == "Oscillator")     return std::make_unique<OscProcessor>();
        if (name == "Simpler")        return std::make_unique<SimplerProcessor>();
        if (name == "DrumRack")       return std::make_unique<DrumRackProcessor>();
        if (name == "FMSynth")        return std::make_unique<FMSynthProcessor>();
        if (name == "WavetableSynth") return std::make_unique<WavetableSynthProcessor>();
        if (name == "KarplusStrong")  return std::make_unique<KarplusStrongProcessor>();
        return nullptr;
    }

    static juce::StringArray getAvailableInstruments() {
        return { "Oscillator", "Simpler", "DrumRack",
                 "FMSynth", "WavetableSynth", "KarplusStrong" };
    }
};
