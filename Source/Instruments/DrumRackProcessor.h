#pragma once
#include <JuceHeader.h>
#include <atomic>
#include <array>
#include "Instrument.h"
#include "LockFreeQueue.h"

struct DrumRackCommand {
    enum Type { LoadBuffer };
    Type type;
    int padIndex;
    juce::AudioBuffer<float>* newBuffer;
};

class DrumRackProcessor : public InstrumentProcessor {
public:
    struct PadSettings {
        std::atomic<float> gain { 1.0f };
        std::atomic<float> pan { 0.5f };
        std::atomic<float> pitchOffset { 0.0f };
        std::atomic<float> decay { 0.5f }; // Seconds
        std::atomic<int> chokeGroup { 0 }; // 0 = none
        juce::File loadedFile;
    };

    struct PadVoice {
        bool active = false;
        int padIndex = -1;
        int playhead = 0;
        float currentGain = 1.0f;
        float decayEnvelope = 1.0f;
        float decayRate = 0.0f;
    };

    DrumRackProcessor() {
        for (int i = 0; i < 16; ++i) {
            padBuffers[i].store(nullptr);
            
            // Set up default choke groups for Hi-Hats (GM mapping: 42=Closed, 44=Pedal, 46=Open)
            // If mapped to C1 (36) starting index:
            // Closed Hat = 36 + 6 = 42 -> Pad 6
            // Pedal Hat = 36 + 8 = 44 -> Pad 8
            // Open Hat = 36 + 10 = 46 -> Pad 10
            if (i == 6 || i == 8 || i == 10) {
                settings[i].chokeGroup.store(1);
            }
        }
    }

    ~DrumRackProcessor() override {
        clear();
    }

    void prepareToPlay(double sampleRate) override {
        currentSampleRate = sampleRate;
    }
    
    bool loadFile(const juce::File& file) override {
        // Ignored for DrumRack, as files are loaded per-pad via commands
        juce::ignoreUnused(file);
        return false;
    }

    void loadBufferToPad(int padIndex, juce::AudioBuffer<float>* buffer, const juce::File& file) {
        if (padIndex >= 0 && padIndex < 16) {
            settings[padIndex].loadedFile = file;
            commandQueue.push({DrumRackCommand::LoadBuffer, padIndex, buffer});
        } else {
            delete buffer;
        }
    }

    void processBlock(juce::AudioBuffer<float>& outBuffer, const juce::MidiBuffer& midiMessages) override {
        // Handle incoming buffers
        while (auto optCmd = commandQueue.pop()) {
            if (optCmd->type == DrumRackCommand::LoadBuffer) {
                auto* old = padBuffers[optCmd->padIndex].exchange(optCmd->newBuffer, std::memory_order_acq_rel);
                if (old != nullptr) {
                    garbageQueue.push(old);
                }
            }
        }

        int numSamples = outBuffer.getNumSamples();
        int currentSampleIndex = 0;

        for (const auto meta : midiMessages) {
            auto msg = meta.getMessage();
            int samplePos = meta.samplePosition;

            if (samplePos > currentSampleIndex) {
                renderAudio(outBuffer, currentSampleIndex, samplePos - currentSampleIndex);
            }
            currentSampleIndex = samplePos;

            if (msg.isNoteOn()) {
                int note = msg.getNoteNumber();
                if (note >= 36 && note <= 51) {
                    triggerPad(note - 36, msg.getFloatVelocity());
                }
            }
        }

        if (currentSampleIndex < numSamples) {
            renderAudio(outBuffer, currentSampleIndex, numSamples - currentSampleIndex);
        }
    }

    void clear() override {
        while (auto opt = commandQueue.pop()) {}
        while (auto opt = garbageQueue.pop()) { delete *opt; }
        
        for (int i = 0; i < 16; ++i) {
            if (auto* old = padBuffers[i].exchange(nullptr, std::memory_order_acq_rel)) {
                delete old;
            }
            for (auto& v : voices) {
                v.active = false;
            }
        }
    }
    
    void processGarbage() override {
        while (auto opt = garbageQueue.pop()) {
            delete *opt;
        }
    }

    juce::ValueTree saveState() const override {
        juce::ValueTree tree("DrumRackState");
        for (int i = 0; i < 16; ++i) {
            if (settings[i].loadedFile.existsAsFile()) {
                juce::ValueTree padNode("Pad");
                padNode.setProperty("index", i, nullptr);
                padNode.setProperty("file_path", settings[i].loadedFile.getFullPathName(), nullptr);
                padNode.setProperty("gain", settings[i].gain.load(), nullptr);
                padNode.setProperty("pan", settings[i].pan.load(), nullptr);
                padNode.setProperty("pitch", settings[i].pitchOffset.load(), nullptr);
                padNode.setProperty("decay", settings[i].decay.load(), nullptr);
                padNode.setProperty("choke", settings[i].chokeGroup.load(), nullptr);
                tree.addChild(padNode, -1, nullptr);
            }
        }
        return tree;
    }

    void loadState(const juce::ValueTree& tree) override {
        // Actual file loading must be triggered from the MainComponent
        // This only restores parameters
        for (int i = 0; i < tree.getNumChildren(); ++i) {
            auto padNode = tree.getChild(i);
            if (padNode.hasType("Pad")) {
                int idx = padNode.getProperty("index", -1);
                if (idx >= 0 && idx < 16) {
                    settings[idx].gain.store(padNode.getProperty("gain", 1.0f));
                    settings[idx].pan.store(padNode.getProperty("pan", 0.5f));
                    settings[idx].pitchOffset.store(padNode.getProperty("pitch", 0.0f));
                    settings[idx].decay.store(padNode.getProperty("decay", 0.5f));
                    settings[idx].chokeGroup.store(padNode.getProperty("choke", 0));
                    settings[idx].loadedFile = juce::File(padNode.getProperty("file_path", ""));
                }
            }
        }
    }

    std::unique_ptr<juce::Component> createEditor() override;
    
    juce::String getName() const override { return "DrumRack"; }
    
    // UI Accessors
    std::array<PadSettings, 16> settings;
    std::atomic<juce::AudioBuffer<float>*> padBuffers[16];

private:
    void triggerPad(int padIndex, float velocity) {
        int chokeGroup = settings[padIndex].chokeGroup.load();
        
        // Find a free voice or steal the oldest
        int voiceIdx = -1;
        for (int i = 0; i < 32; ++i) {
            if (!voices[i].active) {
                voiceIdx = i;
                break;
            }
        }
        if (voiceIdx == -1) voiceIdx = 0; // Simple stealing
        
        // Choke other voices in the same group
        if (chokeGroup > 0) {
            for (int i = 0; i < 32; ++i) {
                if (voices[i].active && voices[i].padIndex != padIndex) {
                    if (settings[voices[i].padIndex].chokeGroup.load() == chokeGroup) {
                        voices[i].active = false;
                    }
                }
            }
        }

        // Initialize voice
        voices[voiceIdx].active = true;
        voices[voiceIdx].padIndex = padIndex;
        voices[voiceIdx].playhead = 0;
        voices[voiceIdx].currentGain = velocity * settings[padIndex].gain.load();
        voices[voiceIdx].decayEnvelope = 1.0f;
        
        float decayTime = settings[padIndex].decay.load();
        voices[voiceIdx].decayRate = 1.0f / (decayTime * currentSampleRate);
    }

    void renderAudio(juce::AudioBuffer<float>& outBuffer, int startSample, int numSamples) {
        for (int v = 0; v < 32; ++v) {
            if (!voices[v].active) continue;
            
            int padIdx = voices[v].padIndex;
            auto* buffer = padBuffers[padIdx].load(std::memory_order_acquire);
            if (buffer == nullptr) {
                voices[v].active = false;
                continue;
            }

            int bufferLen = buffer->getNumSamples();
            int remaining = bufferLen - voices[v].playhead;
            int samplesToProcess = juce::jmin(numSamples, remaining);

            if (samplesToProcess > 0) {
                float pan = settings[padIdx].pan.load();
                float lGain = std::cos(pan * juce::MathConstants<float>::halfPi) * voices[v].currentGain;
                float rGain = std::sin(pan * juce::MathConstants<float>::halfPi) * voices[v].currentGain;

                for (int i = 0; i < samplesToProcess; ++i) {
                    float sampleL = buffer->getSample(0, voices[v].playhead) * voices[v].decayEnvelope;
                    float sampleR = buffer->getSample(juce::jmin(1, buffer->getNumChannels() - 1), voices[v].playhead) * voices[v].decayEnvelope;

                    outBuffer.addSample(0, startSample + i, sampleL * lGain);
                    outBuffer.addSample(1, startSample + i, sampleR * rGain);

                    voices[v].playhead++;
                    voices[v].decayEnvelope -= voices[v].decayRate;
                    if (voices[v].decayEnvelope <= 0.0f) {
                        voices[v].active = false;
                        break;
                    }
                }
            } else {
                voices[v].active = false;
            }
        }
    }

    double currentSampleRate = 44100.0;
    LockFreeQueue<DrumRackCommand, 64> commandQueue;
    LockFreeQueue<juce::AudioBuffer<float>*, 64> garbageQueue;
    
    PadVoice voices[32]; // 32 polyphony
};
