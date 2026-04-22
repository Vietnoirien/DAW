#include "BuiltInEffects.h"

// ─── Generic Parameter Editor ────────────────────────────────────────────────
class GenericEffectEditor : public juce::Component {
public:
    struct Param {
        juce::String name;
        std::atomic<float>* value;
        float min, max;
    };

    GenericEffectEditor(juce::String title, std::vector<Param> parameters) 
        : effectTitle(title), params(parameters) 
    {
        for (auto& p : params) {
            auto* slider = new juce::Slider(juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox);
            slider->setRange(p.min, p.max);
            slider->setValue(p.value->load());
            slider->setDoubleClickReturnValue(true, p.value->load());
            slider->onValueChange = [slider, p]() {
                p.value->store((float)slider->getValue(), std::memory_order_relaxed);
            };
            addAndMakeVisible(slider);
            sliders.add(slider);
        }
        setSize(80 + params.size() * 60, 140);
    }

    void paint(juce::Graphics& g) override {
        g.fillAll(juce::Colour(0xff222222));
        g.setColour(juce::Colour(0xff444444));
        g.drawRect(getLocalBounds(), 2);
        
        g.setColour(juce::Colours::white);
        g.setFont(juce::Font(juce::FontOptions(14.0f, juce::Font::bold)));
        g.drawText(effectTitle, 0, 10, getWidth(), 20, juce::Justification::centred);

        g.setFont(juce::Font(juce::FontOptions(10.0f)));
        for (int i = 0; i < params.size(); ++i) {
            g.drawText(params[i].name, 10 + i * 60, 100, 60, 20, juce::Justification::centred);
        }
    }

    void resized() override {
        for (int i = 0; i < sliders.size(); ++i) {
            sliders[i]->setBounds(15 + i * 60, 40, 50, 50);
        }
    }

private:
    juce::String effectTitle;
    std::vector<Param> params;
    juce::OwnedArray<juce::Slider> sliders;
};

// ─── ReverbEffect ────────────────────────────────────────────────────────────
ReverbEffect::ReverbEffect() {
    params.roomSize = 0.5f;
    params.damping = 0.5f;
    params.wetLevel = 0.33f;
    params.dryLevel = 1.0f;
    params.width = 1.0f;
    params.freezeMode = 0.0f;
    reverb.setParameters(params);
}

void ReverbEffect::prepareToPlay(double sampleRate) {
    juce::dsp::ProcessSpec spec;
    spec.sampleRate = sampleRate;
    spec.maximumBlockSize = 8192;
    spec.numChannels = 2;
    reverb.prepare(spec);
}

void ReverbEffect::processBlock(juce::AudioBuffer<float>& buffer) {
    params.roomSize = roomSize.load(std::memory_order_relaxed);
    params.damping = damping.load(std::memory_order_relaxed);
    params.wetLevel = wetLevel.load(std::memory_order_relaxed);
    params.dryLevel = dryLevel.load(std::memory_order_relaxed);
    params.width = width.load(std::memory_order_relaxed);
    reverb.setParameters(params);

    juce::dsp::AudioBlock<float> block(buffer);
    juce::dsp::ProcessContextReplacing<float> context(block);
    reverb.process(context);
}

void ReverbEffect::clear() {
    reverb.reset();
}

juce::ValueTree ReverbEffect::saveState() const {
    juce::ValueTree state("ReverbState");
    state.setProperty("roomSize", roomSize.load(), nullptr);
    state.setProperty("damping", damping.load(), nullptr);
    state.setProperty("wetLevel", wetLevel.load(), nullptr);
    state.setProperty("dryLevel", dryLevel.load(), nullptr);
    state.setProperty("width", width.load(), nullptr);
    return state;
}

void ReverbEffect::loadState(const juce::ValueTree& tree) {
    if (tree.hasProperty("roomSize")) roomSize.store(tree.getProperty("roomSize"), std::memory_order_relaxed);
    if (tree.hasProperty("damping")) damping.store(tree.getProperty("damping"), std::memory_order_relaxed);
    if (tree.hasProperty("wetLevel")) wetLevel.store(tree.getProperty("wetLevel"), std::memory_order_relaxed);
    if (tree.hasProperty("dryLevel")) dryLevel.store(tree.getProperty("dryLevel"), std::memory_order_relaxed);
    if (tree.hasProperty("width")) width.store(tree.getProperty("width"), std::memory_order_relaxed);
}

std::unique_ptr<juce::Component> ReverbEffect::createEditor() {
    return std::make_unique<GenericEffectEditor>("Reverb", std::vector<GenericEffectEditor::Param>{
        {"Size", &roomSize, 0.0f, 1.0f},
        {"Damp", &damping, 0.0f, 1.0f},
        {"Mix", &wetLevel, 0.0f, 1.0f}
    });
}

// ─── DelayEffect ─────────────────────────────────────────────────────────────
DelayEffect::DelayEffect() {}

void DelayEffect::prepareToPlay(double sampleRate) {
    currentSampleRate = sampleRate;
    juce::dsp::ProcessSpec spec;
    spec.sampleRate = sampleRate;
    spec.maximumBlockSize = 8192;
    spec.numChannels = 2;
    delayLine.prepare(spec);
}

void DelayEffect::processBlock(juce::AudioBuffer<float>& buffer) {
    const int numChannels = buffer.getNumChannels();
    const int numSamples = buffer.getNumSamples();
    
    float fb = feedback.load(std::memory_order_relaxed);
    float mx = mix.load(std::memory_order_relaxed);
    float dTimeSamples = (delayTimeMs.load(std::memory_order_relaxed) / 1000.0f) * currentSampleRate;

    delayLine.setDelay(dTimeSamples);

    for (int channel = 0; channel < numChannels; ++channel) {
        float* channelData = buffer.getWritePointer(channel);

        for (int i = 0; i < numSamples; ++i) {
            float input = channelData[i];
            float delayed = delayLine.popSample(channel);
            
            float toPush = input + delayed * fb;
            delayLine.pushSample(channel, toPush);
            
            channelData[i] = input * (1.0f - mx) + delayed * mx;
        }
    }
}

void DelayEffect::clear() {
    delayLine.reset();
}

juce::ValueTree DelayEffect::saveState() const {
    juce::ValueTree state("DelayState");
    state.setProperty("delayTimeMs", delayTimeMs.load(), nullptr);
    state.setProperty("feedback", feedback.load(), nullptr);
    state.setProperty("mix", mix.load(), nullptr);
    return state;
}

void DelayEffect::loadState(const juce::ValueTree& tree) {
    if (tree.hasProperty("delayTimeMs")) delayTimeMs.store(tree.getProperty("delayTimeMs"), std::memory_order_relaxed);
    if (tree.hasProperty("feedback")) feedback.store(tree.getProperty("feedback"), std::memory_order_relaxed);
    if (tree.hasProperty("mix")) mix.store(tree.getProperty("mix"), std::memory_order_relaxed);
}

std::unique_ptr<juce::Component> DelayEffect::createEditor() {
    return std::make_unique<GenericEffectEditor>("Delay", std::vector<GenericEffectEditor::Param>{
        {"Time ms", &delayTimeMs, 10.0f, 1000.0f},
        {"Fdbk", &feedback, 0.0f, 0.95f},
        {"Mix", &mix, 0.0f, 1.0f}
    });
}

// ─── ChorusEffect ────────────────────────────────────────────────────────────
ChorusEffect::ChorusEffect() {}

void ChorusEffect::prepareToPlay(double sampleRate) {
    juce::dsp::ProcessSpec spec;
    spec.sampleRate = sampleRate;
    spec.maximumBlockSize = 8192;
    spec.numChannels = 2;
    chorus.prepare(spec);
}

void ChorusEffect::processBlock(juce::AudioBuffer<float>& buffer) {
    chorus.setRate(rate.load(std::memory_order_relaxed));
    chorus.setDepth(depth.load(std::memory_order_relaxed));
    chorus.setCentreDelay(centreDelay.load(std::memory_order_relaxed));
    chorus.setFeedback(feedback.load(std::memory_order_relaxed));
    chorus.setMix(mix.load(std::memory_order_relaxed));

    juce::dsp::AudioBlock<float> block(buffer);
    juce::dsp::ProcessContextReplacing<float> context(block);
    chorus.process(context);
}

void ChorusEffect::clear() {
    chorus.reset();
}

juce::ValueTree ChorusEffect::saveState() const {
    juce::ValueTree state("ChorusState");
    state.setProperty("rate", rate.load(), nullptr);
    state.setProperty("depth", depth.load(), nullptr);
    state.setProperty("centreDelay", centreDelay.load(), nullptr);
    state.setProperty("feedback", feedback.load(), nullptr);
    state.setProperty("mix", mix.load(), nullptr);
    return state;
}

void ChorusEffect::loadState(const juce::ValueTree& tree) {
    if (tree.hasProperty("rate")) rate.store(tree.getProperty("rate"), std::memory_order_relaxed);
    if (tree.hasProperty("depth")) depth.store(tree.getProperty("depth"), std::memory_order_relaxed);
    if (tree.hasProperty("centreDelay")) centreDelay.store(tree.getProperty("centreDelay"), std::memory_order_relaxed);
    if (tree.hasProperty("feedback")) feedback.store(tree.getProperty("feedback"), std::memory_order_relaxed);
    if (tree.hasProperty("mix")) mix.store(tree.getProperty("mix"), std::memory_order_relaxed);
}

std::unique_ptr<juce::Component> ChorusEffect::createEditor() {
    return std::make_unique<GenericEffectEditor>("Chorus", std::vector<GenericEffectEditor::Param>{
        {"Rate", &rate, 0.0f, 99.0f},
        {"Depth", &depth, 0.0f, 1.0f},
        {"Delay", &centreDelay, 1.0f, 100.0f},
        {"Mix", &mix, 0.0f, 1.0f}
    });
}

// ─── FilterEffect ────────────────────────────────────────────────────────────
FilterEffect::FilterEffect() {
    filter.setType(juce::dsp::StateVariableTPTFilterType::lowpass);
}

void FilterEffect::prepareToPlay(double sampleRate) {
    juce::dsp::ProcessSpec spec;
    spec.sampleRate = sampleRate;
    spec.maximumBlockSize = 8192;
    spec.numChannels = 2;
    filter.prepare(spec);
}

void FilterEffect::processBlock(juce::AudioBuffer<float>& buffer) {
    filter.setCutoffFrequency(cutoff.load(std::memory_order_relaxed));
    filter.setResonance(resonance.load(std::memory_order_relaxed));

    juce::dsp::AudioBlock<float> block(buffer);
    juce::dsp::ProcessContextReplacing<float> context(block);
    filter.process(context);
}

void FilterEffect::clear() {
    filter.reset();
}

juce::ValueTree FilterEffect::saveState() const {
    juce::ValueTree state("FilterState");
    state.setProperty("cutoff", cutoff.load(), nullptr);
    state.setProperty("resonance", resonance.load(), nullptr);
    return state;
}

void FilterEffect::loadState(const juce::ValueTree& tree) {
    if (tree.hasProperty("cutoff")) cutoff.store(tree.getProperty("cutoff"), std::memory_order_relaxed);
    if (tree.hasProperty("resonance")) resonance.store(tree.getProperty("resonance"), std::memory_order_relaxed);
}

std::unique_ptr<juce::Component> FilterEffect::createEditor() {
    return std::make_unique<GenericEffectEditor>("Filter", std::vector<GenericEffectEditor::Param>{
        {"Cutoff", &cutoff, 20.0f, 20000.0f},
        {"Res", &resonance, 0.1f, 10.0f}
    });
}

