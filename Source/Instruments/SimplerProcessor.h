#pragma once
#include <JuceHeader.h>
#include <atomic>
#include "LockFreeQueue.h"

struct SimplerCommand {
    enum Type { LoadBuffer };
    Type type;
    juce::AudioBuffer<float>* newBuffer;
};

#include "Instrument.h"

class SimplerProcessor : public InstrumentProcessor {
public:
    void prepareToPlay(double sampleRate) override {
        juce::ignoreUnused(sampleRate);
    }
    
    bool loadFile(const juce::File& file) override {
        loadedFile = file;
        return true;
    }
    
    void processBlock(juce::AudioBuffer<float>& outBuffer, const juce::MidiBuffer& midiMessages) override {
        // Handle incoming buffers
        while (auto optCmd = commandQueue.pop()) {
            if (optCmd->type == SimplerCommand::LoadBuffer) {
                auto* old = activeBuffer.exchange(optCmd->newBuffer, std::memory_order_acq_rel);
                if (old != nullptr) {
                    garbageQueue.push(old);
                }
            }
        }

        auto* currentBuffer = activeBuffer.load(std::memory_order_acquire);
        if (currentBuffer == nullptr) return;

        int numSamples = outBuffer.getNumSamples();
        int currentSampleIndex = 0;

        for (const auto meta : midiMessages) {
            auto msg = meta.getMessage();
            int samplePos = meta.samplePosition;

            // Render audio up to (but not including) this event's sample position.
            if (isPlaying && samplePos > currentSampleIndex) {
                renderAudio(outBuffer, currentBuffer, currentSampleIndex, samplePos - currentSampleIndex);
            }

            // BUG-3 FIX: always advance the output cursor to samplePos, regardless
            // of isPlaying. If we don't do this and isPlaying goes false inside
            // renderAudio (sample reached its end mid-block), the NoteOn below
            // will restart from playhead=0 but currentSampleIndex stays at its
            // old value — causing the next renderAudio call to write into the wrong
            // outBuffer offset, producing a full-block phase error.
            currentSampleIndex = samplePos;

            if (msg.isNoteOn()) {
                playhead = 0;
                isPlaying = true;
            } else if (msg.isNoteOff()) {
                // One-shot mode: NoteOff is ignored; sample plays to completion.
            }
        }

        // Render remaining block
        if (isPlaying && currentSampleIndex < numSamples) {
            renderAudio(outBuffer, currentBuffer, currentSampleIndex, numSamples - currentSampleIndex);
        }
    }

    void loadNewBuffer(juce::AudioBuffer<float>* newBuf) {
        commandQueue.push({SimplerCommand::LoadBuffer, newBuf});
    }

    void processGarbage() override {
        while (auto opt = garbageQueue.pop()) {
            delete *opt;
        }
    }

    void clear() override {
        while (auto opt = commandQueue.pop()) {}
        while (auto opt = garbageQueue.pop()) { delete *opt; }
        if (auto* old = activeBuffer.exchange(nullptr, std::memory_order_acq_rel)) {
            delete old;
        }
        playhead = 0;
        isPlaying = false;
    }

    juce::ValueTree saveState() const override {
        return juce::ValueTree("SimplerState");
    }

    void loadState(const juce::ValueTree& tree) override {
        juce::ignoreUnused(tree);
    }
    
    void registerAutomationParameters(AutomationRegistry* registry) override {
        // Simpler currently has no automatable parameters
        juce::ignoreUnused(registry);
    }

    std::unique_ptr<juce::Component> createEditor() override;
    
    juce::String getName() const override { return "Simpler"; }

    void moveFrom(SimplerProcessor& other) {
        clear();
        while (auto opt = other.commandQueue.pop()) commandQueue.push(*opt);
        while (auto opt = other.garbageQueue.pop()) garbageQueue.push(*opt);
        activeBuffer.store(other.activeBuffer.exchange(nullptr, std::memory_order_acq_rel), std::memory_order_release);
        playhead = other.playhead;
        isPlaying = other.isPlaying;
        loadedFile = other.loadedFile;
        other.playhead = 0;
        other.isPlaying = false;
        other.loadedFile = juce::File();
    }

    juce::File loadedFile;

private:
    void renderAudio(juce::AudioBuffer<float>& outBuffer, juce::AudioBuffer<float>* sampleBuffer, int startSample, int numSamples) {
        int remaining = sampleBuffer->getNumSamples() - playhead;
        int samplesToCopy = juce::jmin(numSamples, remaining);

        if (samplesToCopy > 0) {
            for (int ch = 0; ch < outBuffer.getNumChannels(); ++ch) {
                int srcCh = juce::jmin(ch, sampleBuffer->getNumChannels() - 1);
                outBuffer.addFrom(ch, startSample, *sampleBuffer, srcCh, playhead, samplesToCopy);
            }
            playhead += samplesToCopy;
        }

        if (playhead >= sampleBuffer->getNumSamples()) {
            isPlaying = false; // Stopped playing at end of sample
        }
    }

    LockFreeQueue<SimplerCommand, 32> commandQueue;
    LockFreeQueue<juce::AudioBuffer<float>*, 32> garbageQueue;
    std::atomic<juce::AudioBuffer<float>*> activeBuffer {nullptr};
    
    int playhead = 0;
    bool isPlaying = false;
};
