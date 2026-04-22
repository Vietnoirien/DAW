#pragma once
#include <JuceHeader.h>
#include <memory>

/**
 * Base interface for all internal DAW instruments (Synthesizers, Samplers).
 */
class InstrumentProcessor {
public:
    virtual ~InstrumentProcessor() = default;

    // DSP processing
    virtual void prepareToPlay(double sampleRate) = 0;
    virtual void processBlock(juce::AudioBuffer<float>& outBuffer, const juce::MidiBuffer& midiMessages) = 0;
    
    // Clear held voices or internal state (e.g., on track stop)
    virtual void clear() = 0;
    
    // Garbage collection on the message thread
    virtual void processGarbage() {}

    // State persistence
    virtual juce::ValueTree saveState() const = 0;
    virtual void loadState(const juce::ValueTree& tree) = 0;

    // UI Creation
    virtual std::unique_ptr<juce::Component> createEditor() = 0;

    // Identity
    virtual juce::String getName() const = 0;

    // Optional: for Samplers (returns true if successfully handled)
    virtual bool loadFile(const juce::File& file) { 
        juce::ignoreUnused(file);
        return false; 
    }
};
