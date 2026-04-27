#pragma once
#include <JuceHeader.h>
#include <memory>
#include "../Core/ClipData.h"

/**
 * Base interface for all modular audio effects.
 */
class EffectProcessor {
public:
    virtual ~EffectProcessor() = default;

    // DSP processing
    virtual void prepareToPlay(double sampleRate) = 0;
    
    // Note: Since effects process audio in-place or from previous stages, 
    // we take the buffer by reference and modify it.
    virtual void processBlock(juce::AudioBuffer<float>& buffer) = 0;

    // Returns the latency introduced by this effect in samples.
    // Lookahead compressors, linear-phase EQs, etc. should override this.
    virtual int getLatencySamples() const { return 0; }

    // ── Sidechain API ──────────────────────────────────────────────────────
    // Override wantsSidechain() to return true to opt in.
    // The render thread will call setSidechainBuffer() before processBlock()
    // with the pre-rendered audio of the sidechain source track (read-only).
    // When no source is selected (or the source was just deleted) the pointer
    // is null — the effect must handle null gracefully (fall back to self-keying).
    virtual bool wantsSidechain() const { return false; }
    virtual void setSidechainBuffer(const juce::AudioBuffer<float>* /*scBuf*/) {}
    
    // Clear internal state (e.g., reverb tails)
    virtual void clear() = 0;
    
    // Garbage collection on the message thread
    virtual void processGarbage() {}

    // State persistence
    virtual juce::ValueTree saveState() const = 0;
    virtual void loadState(const juce::ValueTree& tree) = 0;

    // Automation Registry
    virtual void registerAutomationParameters(AutomationRegistry* registry) = 0;

    // UI Creation
    virtual std::unique_ptr<juce::Component> createEditor() = 0;

    // Identity
    virtual juce::String getName() const = 0;
};
