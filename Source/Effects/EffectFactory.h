#pragma once
#include "BuiltInEffects.h"

class EffectFactory {
public:
    static std::unique_ptr<EffectProcessor> create(const juce::String& type) {
        if (type == "Reverb") return std::make_unique<ReverbEffect>();
        if (type == "Delay")  return std::make_unique<DelayEffect>();
        if (type == "Chorus") return std::make_unique<ChorusEffect>();
        if (type == "Filter") return std::make_unique<FilterEffect>();
        if (type == "Compressor") return std::make_unique<CompressorEffect>();
        if (type == "Limiter") return std::make_unique<LimiterEffect>();
        if (type == "Phaser") return std::make_unique<PhaserEffect>();
        if (type == "Saturation") return std::make_unique<SaturationEffect>();
        if (type == "ParametricEQ") return std::make_unique<ParametricEQEffect>();
        if (type == "Gain")          return std::make_unique<GainEffect>();
        if (type == "TransientShaper") return std::make_unique<TransientShaperEffect>();
        if (type == "NoiseGate")     return std::make_unique<NoiseGateEffect>();
        return nullptr;
    }
};
