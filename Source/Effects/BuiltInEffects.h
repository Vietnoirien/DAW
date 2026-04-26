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

    // Audio FIFO for real-time spectrum display
    static constexpr int fftOrder = 10;
    static constexpr int fftSize  = 1024;
    juce::AbstractFifo audioFifo { 4096 };
    std::array<float, 4096> audioFifoData;

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

    static constexpr int fftOrder = 10;
    static constexpr int fftSize  = 1024;
    juce::AbstractFifo audioFifo { 4096 };
    std::array<float, 4096> audioFifoData;

private:
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear> delayLine { 192000 };
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

    static constexpr int fftOrder = 10;
    static constexpr int fftSize  = 1024;
    juce::AbstractFifo audioFifo { 4096 };
    std::array<float, 4096> audioFifoData;

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

    static constexpr int fftOrder = 10;
    static constexpr int fftSize  = 1024;
    juce::AbstractFifo audioFifo { 4096 };
    std::array<float, 4096> audioFifoData;
    double currentSampleRate = 44100.0;

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

    // Real-time level/GR for meter display
    std::atomic<float> inputLevelDb  { -100.0f };
    std::atomic<float> gainReductionDb { 0.0f };

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

    // Real-time level for meter display
    std::atomic<float> inputLevelDb  { -100.0f };
    std::atomic<float> outputLevelDb { -100.0f };

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

    static constexpr int fftOrder = 10;
    static constexpr int fftSize  = 1024;
    juce::AbstractFifo audioFifo { 4096 };
    std::array<float, 4096> audioFifoData;

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

    static constexpr int fftOrder = 10;
    static constexpr int fftSize  = 1024;
    juce::AbstractFifo audioFifo { 4096 };
    std::array<float, 4096> audioFifoData;

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
// ─── GainEffect ──────────────────────────────────────────────────────────────
class GainEffect : public EffectProcessor {
public:
    GainEffect();
    ~GainEffect() override = default;

    void prepareToPlay(double sampleRate) override;
    void processBlock(juce::AudioBuffer<float>& buffer) override;
    void clear() override;

    juce::ValueTree saveState() const override;
    void loadState(const juce::ValueTree& tree) override;
    void registerAutomationParameters(AutomationRegistry* registry) override;

    std::unique_ptr<juce::Component> createEditor() override;
    juce::String getName() const override { return "Gain"; }

    std::atomic<float> gainDb { 0.0f };
    std::atomic<float> pan    { 0.0f };

    // FIFO for level meter display
    static constexpr int fftOrder = 10;
    static constexpr int fftSize  = 1024;
    juce::AbstractFifo inputFifo  { 4096 };
    juce::AbstractFifo outputFifo { 4096 };
    std::array<float, 4096> inputFifoData;
    std::array<float, 4096> outputFifoData;
    std::atomic<float> inputLevelDb  { -100.0f };
    std::atomic<float> outputLevelDb { -100.0f };

private:
    juce::dsp::Gain<float> gainProc;
};

// ─── TransientShaperEffect ───────────────────────────────────────────────────
class TransientShaperEffect : public EffectProcessor {
public:
    TransientShaperEffect();
    ~TransientShaperEffect() override = default;

    void prepareToPlay(double sampleRate) override;
    void processBlock(juce::AudioBuffer<float>& buffer) override;
    void clear() override;

    juce::ValueTree saveState() const override;
    void loadState(const juce::ValueTree& tree) override;
    void registerAutomationParameters(AutomationRegistry* registry) override;

    std::unique_ptr<juce::Component> createEditor() override;
    juce::String getName() const override { return "TransientShaper"; }

    std::atomic<float> attackAmt     { 0.0f };  // -1..+1
    std::atomic<float> sustainAmt    { 0.0f };  // -1..+1
    std::atomic<float> sensitivity   { 1.0f };  // 0.1..10

    // Envelope state visible to the editor for display
    std::atomic<float> fastEnvDisplay  { 0.0f };
    std::atomic<float> slowEnvDisplay  { 0.0f };

    // FIFO for spectrum
    static constexpr int fftOrder = 10;
    static constexpr int fftSize  = 1024;
    juce::AbstractFifo audioFifo { 4096 };
    std::array<float, 4096> audioFifoData;

private:
    // Per-channel fast/slow envelope follower state
    std::array<float, 2> fastEnv { 0.0f, 0.0f };
    std::array<float, 2> slowEnv { 0.0f, 0.0f };
    double currentSampleRate = 44100.0;
    // IIR coefficients for the two followers
    float alphaFast = 0.0f;
    float alphaSlow = 0.0f;
};

// ─── NoiseGateEffect ─────────────────────────────────────────────────────────
class NoiseGateEffect : public EffectProcessor {
public:
    NoiseGateEffect();
    ~NoiseGateEffect() override = default;

    void prepareToPlay(double sampleRate) override;
    void processBlock(juce::AudioBuffer<float>& buffer) override;
    void clear() override;

    juce::ValueTree saveState() const override;
    void loadState(const juce::ValueTree& tree) override;
    void registerAutomationParameters(AutomationRegistry* registry) override;

    std::unique_ptr<juce::Component> createEditor() override;
    juce::String getName() const override { return "NoiseGate"; }

    std::atomic<float> threshold { -40.0f }; // dBFS
    std::atomic<float> attackMs  {   5.0f }; // ms
    std::atomic<float> releaseMs { 200.0f }; // ms
    std::atomic<float> holdMs    {  50.0f }; // ms
    std::atomic<float> rangeDb   { -80.0f }; // attenuation when closed

    // Gate state for display
    std::atomic<bool>  gateOpen  { false };
    std::atomic<float> gainReduction { 0.0f }; // 0..1 (1 = fully open)

    // FIFO for spectrum
    static constexpr int fftOrder = 10;
    static constexpr int fftSize  = 1024;
    juce::AbstractFifo audioFifo { 4096 };
    std::array<float, 4096> audioFifoData;

private:
    double currentSampleRate = 44100.0;
    float  envGain = 0.0f;    // current smoothed gate gain (0..1)
    int    holdCounter = 0;   // samples remaining in hold phase
    float  alphaAttack  = 0.0f;
    float  alphaRelease = 0.0f;
};
