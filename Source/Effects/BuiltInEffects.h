#pragma once
#include "EffectProcessor.h"
#include <juce_dsp/juce_dsp.h>

class ReverbEffect : public EffectProcessor {
public:
    ReverbEffect();
    ~ReverbEffect() override = default;

    void prepareToPlay(double sampleRate) override;
    void processBlock(juce::AudioBuffer<float>& buffer) override;
    void clear() override;

    juce::ValueTree saveState() const override;
    void loadState(const juce::ValueTree& tree) override;

    void registerAutomationParameters(AutomationRegistry* registry) override;

    std::unique_ptr<juce::Component> createEditor() override;
    juce::String getName() const override { return "Reverb"; }

    std::atomic<float> roomSize { 0.5f };
    std::atomic<float> damping  { 0.5f };
    std::atomic<float> wetLevel { 0.33f };
    std::atomic<float> dryLevel { 1.0f };
    std::atomic<float> width    { 1.0f };

private:
    juce::dsp::Reverb reverb;
    juce::dsp::Reverb::Parameters params;
};

class DelayEffect : public EffectProcessor {
public:
    DelayEffect();
    ~DelayEffect() override = default;

    void prepareToPlay(double sampleRate) override;
    void processBlock(juce::AudioBuffer<float>& buffer) override;
    void clear() override;

    juce::ValueTree saveState() const override;
    void loadState(const juce::ValueTree& tree) override;

    void registerAutomationParameters(AutomationRegistry* registry) override;

    std::unique_ptr<juce::Component> createEditor() override;
    juce::String getName() const override { return "Delay"; }

    std::atomic<float> delayTimeMs { 250.0f };
    std::atomic<float> feedback { 0.5f };
    std::atomic<float> mix { 0.5f };

private:
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear> delayLine { 192000 }; // Max ~4 sec at 44.1kHz
    double currentSampleRate = 44100.0;
};

class ChorusEffect : public EffectProcessor {
public:
    ChorusEffect();
    ~ChorusEffect() override = default;

    void prepareToPlay(double sampleRate) override;
    void processBlock(juce::AudioBuffer<float>& buffer) override;
    void clear() override;

    juce::ValueTree saveState() const override;
    void loadState(const juce::ValueTree& tree) override;

    void registerAutomationParameters(AutomationRegistry* registry) override;

    std::unique_ptr<juce::Component> createEditor() override;
    juce::String getName() const override { return "Chorus"; }

    std::atomic<float> rate { 1.0f };
    std::atomic<float> depth { 0.25f };
    std::atomic<float> centreDelay { 7.0f };
    std::atomic<float> feedback { 0.0f };
    std::atomic<float> mix { 0.5f };

private:
    juce::dsp::Chorus<float> chorus;
};

class FilterEffect : public EffectProcessor {
public:
    FilterEffect();
    ~FilterEffect() override = default;

    void prepareToPlay(double sampleRate) override;
    void processBlock(juce::AudioBuffer<float>& buffer) override;
    void clear() override;

    juce::ValueTree saveState() const override;
    void loadState(const juce::ValueTree& tree) override;

    void registerAutomationParameters(AutomationRegistry* registry) override;

    std::unique_ptr<juce::Component> createEditor() override;
    juce::String getName() const override { return "Filter"; }

    std::atomic<float> cutoff { 1000.0f };
    std::atomic<float> resonance { 1.0f };

private:
    juce::dsp::StateVariableTPTFilter<float> filter;
};

// ─── CompressorEffect ────────────────────────────────────────────────────────
class CompressorEffect : public EffectProcessor {
public:
    CompressorEffect();
    ~CompressorEffect() override = default;

    void prepareToPlay(double sampleRate) override;
    void processBlock(juce::AudioBuffer<float>& buffer) override;
    void clear() override;

    juce::ValueTree saveState() const override;
    void loadState(const juce::ValueTree& tree) override;

    void registerAutomationParameters(AutomationRegistry* registry) override;

    std::unique_ptr<juce::Component> createEditor() override;
    juce::String getName() const override { return "Compressor"; }

    std::atomic<float> threshold { -10.0f };
    std::atomic<float> ratio { 4.0f };
    std::atomic<float> attack { 10.0f };
    std::atomic<float> release { 100.0f };

private:
    juce::dsp::Compressor<float> compressor;
};

// ─── LimiterEffect ───────────────────────────────────────────────────────────
class LimiterEffect : public EffectProcessor {
public:
    LimiterEffect();
    ~LimiterEffect() override = default;

    void prepareToPlay(double sampleRate) override;
    void processBlock(juce::AudioBuffer<float>& buffer) override;
    void clear() override;

    juce::ValueTree saveState() const override;
    void loadState(const juce::ValueTree& tree) override;

    void registerAutomationParameters(AutomationRegistry* registry) override;

    std::unique_ptr<juce::Component> createEditor() override;
    juce::String getName() const override { return "Limiter"; }

    std::atomic<float> threshold { -1.0f };
    std::atomic<float> release { 50.0f };

private:
    juce::dsp::Limiter<float> limiter;
};

// ─── PhaserEffect ────────────────────────────────────────────────────────────
class PhaserEffect : public EffectProcessor {
public:
    PhaserEffect();
    ~PhaserEffect() override = default;

    void prepareToPlay(double sampleRate) override;
    void processBlock(juce::AudioBuffer<float>& buffer) override;
    void clear() override;

    juce::ValueTree saveState() const override;
    void loadState(const juce::ValueTree& tree) override;

    void registerAutomationParameters(AutomationRegistry* registry) override;

    std::unique_ptr<juce::Component> createEditor() override;
    juce::String getName() const override { return "Phaser"; }

    std::atomic<float> rate { 1.0f };
    std::atomic<float> depth { 0.5f };
    std::atomic<float> centreFreq { 1000.0f };
    std::atomic<float> feedback { 0.5f };
    std::atomic<float> mix { 0.5f };

private:
    juce::dsp::Phaser<float> phaser;
};

// ─── SaturationEffect ────────────────────────────────────────────────────────
class SaturationEffect : public EffectProcessor {
public:
    SaturationEffect();
    ~SaturationEffect() override = default;

    void prepareToPlay(double sampleRate) override;
    void processBlock(juce::AudioBuffer<float>& buffer) override;
    void clear() override;

    juce::ValueTree saveState() const override;
    void loadState(const juce::ValueTree& tree) override;

    void registerAutomationParameters(AutomationRegistry* registry) override;

    std::unique_ptr<juce::Component> createEditor() override;
    juce::String getName() const override { return "Saturation"; }

    std::atomic<float> drive { 1.0f };
    std::atomic<float> mix { 1.0f };

private:
    juce::dsp::WaveShaper<float> waveShaper;
};

// ─── ParametricEQEffect ──────────────────────────────────────────────────────
class ParametricEQEffect : public EffectProcessor {
public:
    ParametricEQEffect();
    ~ParametricEQEffect() override = default;

    void prepareToPlay(double sampleRate) override;
    void processBlock(juce::AudioBuffer<float>& buffer) override;
    void clear() override;

    juce::ValueTree saveState() const override;
    void loadState(const juce::ValueTree& tree) override;

    void registerAutomationParameters(AutomationRegistry* registry) override;

    std::unique_ptr<juce::Component> createEditor() override;
    juce::String getName() const override { return "ParametricEQ"; }

    std::atomic<float> freq1 { 100.0f };
    std::atomic<float> gain1 { 1.0f };
    std::atomic<float> q1    { 0.707f };
    
    std::atomic<float> freq2 { 1000.0f };
    std::atomic<float> gain2 { 1.0f };
    std::atomic<float> q2    { 0.707f };
    
    std::atomic<float> freq3 { 5000.0f };
    std::atomic<float> gain3 { 1.0f };
    std::atomic<float> q3    { 0.707f };

    static constexpr int fftOrder = 11;
    static constexpr int fftSize = 1 << fftOrder;
    juce::AbstractFifo audioFifo { 8192 };
    std::array<float, 8192> audioFifoData;
    
    double getSampleRate() const { return currentSampleRate; }

private:
    juce::dsp::ProcessorChain<
        juce::dsp::IIR::Filter<float>,
        juce::dsp::IIR::Filter<float>,
        juce::dsp::IIR::Filter<float>
    > eqChain;
    double currentSampleRate = 44100.0;
};
