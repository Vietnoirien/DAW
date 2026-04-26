#include "AudioInputProcessor.h"
#include "AudioInputComponent.h"
#include "../Core/MainComponent.h"

AudioInputProcessor::AudioInputProcessor()
{
}

AudioInputProcessor::~AudioInputProcessor()
{
}

void AudioInputProcessor::prepareToPlay(double sampleRate)
{
    currentSampleRate = sampleRate;
}

void AudioInputProcessor::processBlock(juce::AudioBuffer<float>& outBuffer, const juce::MidiBuffer&)
{
    outBuffer.clear();

    if (!hostTrack) return;
    
    // Read from host track's monitorFifo
    if (hostTrack->monitorFifo != nullptr) {
        int numSamples = outBuffer.getNumSamples();
        int numChannels = outBuffer.getNumChannels();
        
        int start1, size1, start2, size2;
        hostTrack->monitorFifo->prepareToRead(numSamples, start1, size1, start2, size2);
        
        float gain = hostTrack->monitorGain.load(std::memory_order_relaxed);

        if (size1 > 0) {
            for (int ch = 0; ch < numChannels; ++ch) {
                float* outData = outBuffer.getWritePointer(ch);
                const float* inData = hostTrack->monitorBuffer.data() + start1;
                for (int i = 0; i < size1; ++i) {
                    outData[i] += inData[i] * gain;
                }
            }
        }
        
        if (size2 > 0) {
            for (int ch = 0; ch < numChannels; ++ch) {
                float* outData = outBuffer.getWritePointer(ch, size1);
                const float* inData = hostTrack->monitorBuffer.data() + start2;
                for (int i = 0; i < size2; ++i) {
                    outData[i] += inData[i] * gain;
                }
            }
        }
        
        hostTrack->monitorFifo->finishedRead(size1 + size2);
    }
}

void AudioInputProcessor::clear()
{
}

void AudioInputProcessor::setHostTrack(void* track)
{
    hostTrack = static_cast<Track*>(track);
}

juce::ValueTree AudioInputProcessor::saveState() const
{
    juce::ValueTree state("AudioInputState");
    if (hostTrack) {
        state.setProperty("monitorEnabled", hostTrack->monitorEnabled.load(), nullptr);
        state.setProperty("monitorGain", hostTrack->monitorGain.load(), nullptr);
        state.setProperty("inputChannel", hostTrack->recordInputChannel.load(), nullptr);
    }
    return state;
}

void AudioInputProcessor::loadState(const juce::ValueTree& tree)
{
    if (!hostTrack) return;
    if (tree.hasProperty("monitorEnabled"))
        hostTrack->monitorEnabled.store((bool)tree.getProperty("monitorEnabled"));
    if (tree.hasProperty("monitorGain"))
        hostTrack->monitorGain.store((float)tree.getProperty("monitorGain"));
    if (tree.hasProperty("inputChannel"))
        hostTrack->recordInputChannel.store((int)tree.getProperty("inputChannel"));
}

void AudioInputProcessor::registerAutomationParameters(AutomationRegistry* registry)
{
    juce::ignoreUnused(registry);
}

std::unique_ptr<juce::Component> AudioInputProcessor::createEditor()
{
    return std::make_unique<AudioInputComponent>(*this);
}
