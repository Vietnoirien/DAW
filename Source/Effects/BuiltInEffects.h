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

    std::unique_ptr<juce::Component> createEditor() override;
    juce::String getName() const override { return "Filter"; }

    std::atomic<float> cutoff { 1000.0f };
    std::atomic<float> resonance { 1.0f };

private:
    juce::dsp::StateVariableTPTFilter<float> filter;
};

