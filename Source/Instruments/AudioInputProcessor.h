#pragma once
#include <JuceHeader.h>
#include "Instrument.h"

// Forward declaration of Track to avoid circular dependency
struct Track;

class AudioInputProcessor : public InstrumentProcessor
{
public:
    AudioInputProcessor();
    ~AudioInputProcessor() override;

    void prepareToPlay(double sampleRate) override;
    void processBlock(juce::AudioBuffer<float>& outBuffer, const juce::MidiBuffer& midiMessages) override;
    
    void clear() override;
    void setHostTrack(void* track) override;

    juce::ValueTree saveState() const override;
    void loadState(const juce::ValueTree& tree) override;

    void registerAutomationParameters(AutomationRegistry* registry) override;

    std::unique_ptr<juce::Component> createEditor() override;
    juce::String getName() const override { return "AudioInput"; }

    // Direct access to host track for UI binding
    Track* getHostTrack() const { return hostTrack; }

private:
    Track* hostTrack = nullptr;
    double currentSampleRate = 44100.0;
};
