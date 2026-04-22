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

// ─── CompressorEffect ────────────────────────────────────────────────────────
CompressorEffect::CompressorEffect() {
    compressor.setThreshold(threshold.load());
    compressor.setRatio(ratio.load());
    compressor.setAttack(attack.load());
    compressor.setRelease(release.load());
}

void CompressorEffect::prepareToPlay(double sampleRate) {
    juce::dsp::ProcessSpec spec;
    spec.sampleRate = sampleRate;
    spec.maximumBlockSize = 8192;
    spec.numChannels = 2;
    compressor.prepare(spec);
}

void CompressorEffect::processBlock(juce::AudioBuffer<float>& buffer) {
    compressor.setThreshold(threshold.load(std::memory_order_relaxed));
    compressor.setRatio(ratio.load(std::memory_order_relaxed));
    compressor.setAttack(attack.load(std::memory_order_relaxed));
    compressor.setRelease(release.load(std::memory_order_relaxed));

    juce::dsp::AudioBlock<float> block(buffer);
    juce::dsp::ProcessContextReplacing<float> context(block);
    compressor.process(context);
}

void CompressorEffect::clear() {
    compressor.reset();
}

juce::ValueTree CompressorEffect::saveState() const {
    juce::ValueTree state("CompressorState");
    state.setProperty("threshold", threshold.load(), nullptr);
    state.setProperty("ratio", ratio.load(), nullptr);
    state.setProperty("attack", attack.load(), nullptr);
    state.setProperty("release", release.load(), nullptr);
    return state;
}

void CompressorEffect::loadState(const juce::ValueTree& tree) {
    if (tree.hasProperty("threshold")) threshold.store(tree.getProperty("threshold"), std::memory_order_relaxed);
    if (tree.hasProperty("ratio")) ratio.store(tree.getProperty("ratio"), std::memory_order_relaxed);
    if (tree.hasProperty("attack")) attack.store(tree.getProperty("attack"), std::memory_order_relaxed);
    if (tree.hasProperty("release")) release.store(tree.getProperty("release"), std::memory_order_relaxed);
}

std::unique_ptr<juce::Component> CompressorEffect::createEditor() {
    return std::make_unique<GenericEffectEditor>("Compressor", std::vector<GenericEffectEditor::Param>{
        {"Thresh", &threshold, -60.0f, 0.0f},
        {"Ratio", &ratio, 1.0f, 20.0f},
        {"Attack", &attack, 0.1f, 100.0f},
        {"Release", &release, 10.0f, 1000.0f}
    });
}

// ─── LimiterEffect ───────────────────────────────────────────────────────────
LimiterEffect::LimiterEffect() {
    limiter.setThreshold(threshold.load());
    limiter.setRelease(release.load());
}

void LimiterEffect::prepareToPlay(double sampleRate) {
    juce::dsp::ProcessSpec spec;
    spec.sampleRate = sampleRate;
    spec.maximumBlockSize = 8192;
    spec.numChannels = 2;
    limiter.prepare(spec);
}

void LimiterEffect::processBlock(juce::AudioBuffer<float>& buffer) {
    limiter.setThreshold(threshold.load(std::memory_order_relaxed));
    limiter.setRelease(release.load(std::memory_order_relaxed));

    juce::dsp::AudioBlock<float> block(buffer);
    juce::dsp::ProcessContextReplacing<float> context(block);
    limiter.process(context);
}

void LimiterEffect::clear() {
    limiter.reset();
}

juce::ValueTree LimiterEffect::saveState() const {
    juce::ValueTree state("LimiterState");
    state.setProperty("threshold", threshold.load(), nullptr);
    state.setProperty("release", release.load(), nullptr);
    return state;
}

void LimiterEffect::loadState(const juce::ValueTree& tree) {
    if (tree.hasProperty("threshold")) threshold.store(tree.getProperty("threshold"), std::memory_order_relaxed);
    if (tree.hasProperty("release")) release.store(tree.getProperty("release"), std::memory_order_relaxed);
}

std::unique_ptr<juce::Component> LimiterEffect::createEditor() {
    return std::make_unique<GenericEffectEditor>("Limiter", std::vector<GenericEffectEditor::Param>{
        {"Thresh", &threshold, -40.0f, 0.0f},
        {"Release", &release, 10.0f, 1000.0f}
    });
}

// ─── PhaserEffect ────────────────────────────────────────────────────────────
PhaserEffect::PhaserEffect() {}

void PhaserEffect::prepareToPlay(double sampleRate) {
    juce::dsp::ProcessSpec spec;
    spec.sampleRate = sampleRate;
    spec.maximumBlockSize = 8192;
    spec.numChannels = 2;
    phaser.prepare(spec);
}

void PhaserEffect::processBlock(juce::AudioBuffer<float>& buffer) {
    phaser.setRate(rate.load(std::memory_order_relaxed));
    phaser.setDepth(depth.load(std::memory_order_relaxed));
    phaser.setCentreFrequency(centreFreq.load(std::memory_order_relaxed));
    phaser.setFeedback(feedback.load(std::memory_order_relaxed));
    phaser.setMix(mix.load(std::memory_order_relaxed));

    juce::dsp::AudioBlock<float> block(buffer);
    juce::dsp::ProcessContextReplacing<float> context(block);
    phaser.process(context);
}

void PhaserEffect::clear() {
    phaser.reset();
}

juce::ValueTree PhaserEffect::saveState() const {
    juce::ValueTree state("PhaserState");
    state.setProperty("rate", rate.load(), nullptr);
    state.setProperty("depth", depth.load(), nullptr);
    state.setProperty("centreFreq", centreFreq.load(), nullptr);
    state.setProperty("feedback", feedback.load(), nullptr);
    state.setProperty("mix", mix.load(), nullptr);
    return state;
}

void PhaserEffect::loadState(const juce::ValueTree& tree) {
    if (tree.hasProperty("rate")) rate.store(tree.getProperty("rate"), std::memory_order_relaxed);
    if (tree.hasProperty("depth")) depth.store(tree.getProperty("depth"), std::memory_order_relaxed);
    if (tree.hasProperty("centreFreq")) centreFreq.store(tree.getProperty("centreFreq"), std::memory_order_relaxed);
    if (tree.hasProperty("feedback")) feedback.store(tree.getProperty("feedback"), std::memory_order_relaxed);
    if (tree.hasProperty("mix")) mix.store(tree.getProperty("mix"), std::memory_order_relaxed);
}

std::unique_ptr<juce::Component> PhaserEffect::createEditor() {
    return std::make_unique<GenericEffectEditor>("Phaser", std::vector<GenericEffectEditor::Param>{
        {"Rate", &rate, 0.01f, 10.0f},
        {"Depth", &depth, 0.0f, 1.0f},
        {"Freq", &centreFreq, 50.0f, 5000.0f},
        {"Fdbk", &feedback, -1.0f, 1.0f},
        {"Mix", &mix, 0.0f, 1.0f}
    });
}

// ─── SaturationEffect ────────────────────────────────────────────────────────
SaturationEffect::SaturationEffect() {
    waveShaper.functionToUse = [](float x) {
        return std::tanh(x);
    };
}

void SaturationEffect::prepareToPlay(double sampleRate) {
    juce::dsp::ProcessSpec spec;
    spec.sampleRate = sampleRate;
    spec.maximumBlockSize = 8192;
    spec.numChannels = 2;
    waveShaper.prepare(spec);
}

void SaturationEffect::processBlock(juce::AudioBuffer<float>& buffer) {
    float drv = drive.load(std::memory_order_relaxed);
    float mx = mix.load(std::memory_order_relaxed);

    juce::AudioBuffer<float> dryBuffer;
    dryBuffer.makeCopyOf(buffer);

    buffer.applyGain(drv);
    
    juce::dsp::AudioBlock<float> block(buffer);
    juce::dsp::ProcessContextReplacing<float> context(block);
    waveShaper.process(context);
    
    buffer.applyGain(1.0f / drv); // Compensate level

    // Mix
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch) {
        auto* writePtr = buffer.getWritePointer(ch);
        auto* dryPtr = dryBuffer.getReadPointer(ch);
        for (int i = 0; i < buffer.getNumSamples(); ++i) {
            writePtr[i] = dryPtr[i] * (1.0f - mx) + writePtr[i] * mx;
        }
    }
}

void SaturationEffect::clear() {
    waveShaper.reset();
}

juce::ValueTree SaturationEffect::saveState() const {
    juce::ValueTree state("SaturationState");
    state.setProperty("drive", drive.load(), nullptr);
    state.setProperty("mix", mix.load(), nullptr);
    return state;
}

void SaturationEffect::loadState(const juce::ValueTree& tree) {
    if (tree.hasProperty("drive")) drive.store(tree.getProperty("drive"), std::memory_order_relaxed);
    if (tree.hasProperty("mix")) mix.store(tree.getProperty("mix"), std::memory_order_relaxed);
}

std::unique_ptr<juce::Component> SaturationEffect::createEditor() {
    return std::make_unique<GenericEffectEditor>("Saturation", std::vector<GenericEffectEditor::Param>{
        {"Drive", &drive, 1.0f, 20.0f},
        {"Mix", &mix, 0.0f, 1.0f}
    });
}

// ─── ParametricEQEditor ──────────────────────────────────────────────────────
class ParametricEQEditor : public juce::Component, private juce::Timer {
public:
    ParametricEQEditor(ParametricEQEffect* effect) 
        : pEffect(effect),
          forwardFFT(ParametricEQEffect::fftOrder),
          window(ParametricEQEffect::fftSize, juce::dsp::WindowingFunction<float>::hann)
    {
        spectrumData.fill(-100.0f);

        setupSlider("F1 Hz", &pEffect->freq1, 20.0f, 500.0f, slidersF[0]);
        setupSlider("G1", &pEffect->gain1, 0.1f, 10.0f, slidersG[0]);
        setupSlider("Q1", &pEffect->q1, 0.1f, 10.0f, slidersQ[0]);

        setupSlider("F2 Hz", &pEffect->freq2, 500.0f, 5000.0f, slidersF[1]);
        setupSlider("G2", &pEffect->gain2, 0.1f, 10.0f, slidersG[1]);
        setupSlider("Q2", &pEffect->q2, 0.1f, 10.0f, slidersQ[1]);

        setupSlider("F3 Hz", &pEffect->freq3, 5000.0f, 20000.0f, slidersF[2]);
        setupSlider("G3", &pEffect->gain3, 0.1f, 10.0f, slidersG[2]);
        setupSlider("Q3", &pEffect->q3, 0.1f, 10.0f, slidersQ[2]);

        setSize(500, 300);
        startTimerHz(30);
    }

    ~ParametricEQEditor() override {
        stopTimer();
    }

    void timerCallback() override {
        if (pEffect->audioFifo.getNumReady() >= ParametricEQEffect::fftSize) {
            std::array<float, ParametricEQEffect::fftSize> fftData;
            int start1, size1, start2, size2;
            pEffect->audioFifo.prepareToRead(ParametricEQEffect::fftSize, start1, size1, start2, size2);
            if (size1 > 0) std::copy(pEffect->audioFifoData.begin() + start1, pEffect->audioFifoData.begin() + start1 + size1, fftData.begin());
            if (size2 > 0) std::copy(pEffect->audioFifoData.begin() + start2, pEffect->audioFifoData.begin() + start2 + size2, fftData.begin() + size1);
            pEffect->audioFifo.finishedRead(size1 + size2);

            window.multiplyWithWindowingTable(fftData.data(), ParametricEQEffect::fftSize);
            std::vector<float> fftBuffer(ParametricEQEffect::fftSize * 2, 0.0f);
            std::copy(fftData.begin(), fftData.end(), fftBuffer.begin());
            
            forwardFFT.performFrequencyOnlyForwardTransform(fftBuffer.data());
            
            auto numBins = ParametricEQEffect::fftSize / 2;
            for (int i = 0; i < spectrumSize; ++i) {
                float freq = juce::mapToLog10(float(i) / spectrumSize, 20.0f, 20000.0f);
                float binIdx = freq * ParametricEQEffect::fftSize / pEffect->getSampleRate();
                int bin = juce::jlimit(0, numBins - 1, (int)binIdx);
                float mag = fftBuffer[bin];
                float db = juce::Decibels::gainToDecibels(mag) - juce::Decibels::gainToDecibels((float)ParametricEQEffect::fftSize);
                spectrumData[i] = spectrumData[i] * 0.7f + db * 0.3f;
            }
            repaint();
        }
    }

    void paint(juce::Graphics& g) override {
        g.fillAll(juce::Colour(0xff222222));
        g.setColour(juce::Colour(0xff444444));
        g.drawRect(getLocalBounds(), 2);
        
        juce::Rectangle<int> graphRect(10, 30, getWidth() - 20, 180);
        g.setColour(juce::Colour(0xff111111));
        g.fillRect(graphRect);

        // draw grid
        g.setColour(juce::Colour(0x44ffffff));
        for (float f = 20.0f; f <= 20000.0f; f *= 10.0f) {
            float x = graphRect.getX() + graphRect.getWidth() * juce::mapFromLog10(f, 20.0f, 20000.0f);
            g.drawVerticalLine((int)x, graphRect.getY(), graphRect.getBottom());
        }

        // draw spectrum
        juce::Path spectrumPath;
        for (int i = 0; i < spectrumSize; ++i) {
            float x = graphRect.getX() + graphRect.getWidth() * ((float)i / spectrumSize);
            float y = juce::jmap(spectrumData[i], -80.0f, 0.0f, (float)graphRect.getBottom(), (float)graphRect.getY());
            y = juce::jlimit((float)graphRect.getY(), (float)graphRect.getBottom(), y);
            if (i == 0) spectrumPath.startNewSubPath(x, y);
            else spectrumPath.lineTo(x, y);
        }
        if (!spectrumPath.isEmpty()) {
            spectrumPath.lineTo(graphRect.getRight(), graphRect.getBottom());
            spectrumPath.lineTo(graphRect.getX(), graphRect.getBottom());
            spectrumPath.closeSubPath();
            g.setColour(juce::Colour(0x8844ffaa));
            g.fillPath(spectrumPath);
        }

        // draw theoretical EQ curve
        juce::Path eqPath;
        auto sr = pEffect->getSampleRate();
        if (sr > 0) {
            auto coef1 = juce::dsp::IIR::Coefficients<float>::makePeakFilter(sr, pEffect->freq1.load(), pEffect->q1.load(), pEffect->gain1.load());
            auto coef2 = juce::dsp::IIR::Coefficients<float>::makePeakFilter(sr, pEffect->freq2.load(), pEffect->q2.load(), pEffect->gain2.load());
            auto coef3 = juce::dsp::IIR::Coefficients<float>::makePeakFilter(sr, pEffect->freq3.load(), pEffect->q3.load(), pEffect->gain3.load());

            for (int x = 0; x < graphRect.getWidth(); ++x) {
                float freq = juce::mapToLog10(float(x) / graphRect.getWidth(), 20.0f, 20000.0f);
                double mag1 = coef1->getMagnitudeForFrequency(freq, sr);
                double mag2 = coef2->getMagnitudeForFrequency(freq, sr);
                double mag3 = coef3->getMagnitudeForFrequency(freq, sr);
                double totalDb = juce::Decibels::gainToDecibels(mag1 * mag2 * mag3);
                
                float y = juce::jmap((float)totalDb, -24.0f, 24.0f, (float)graphRect.getBottom(), (float)graphRect.getY());
                y = juce::jlimit((float)graphRect.getY(), (float)graphRect.getBottom(), y);
                
                float px = graphRect.getX() + x;
                if (x == 0) eqPath.startNewSubPath(px, y);
                else eqPath.lineTo(px, y);
            }
            g.setColour(juce::Colours::white);
            g.strokePath(eqPath, juce::PathStrokeType(2.0f));
        }

        g.setColour(juce::Colours::white);
        g.setFont(juce::Font(juce::FontOptions(14.0f, juce::Font::bold)));
        g.drawText("Parametric EQ", 0, 5, getWidth(), 20, juce::Justification::centred);
        
        g.setFont(juce::Font(juce::FontOptions(10.0f)));
        for (int b = 0; b < 3; ++b) {
            int bx = 20 + b * 150;
            g.drawText("Freq " + juce::String(b+1), bx, 270, 40, 20, juce::Justification::centred);
            g.drawText("Gain " + juce::String(b+1), bx + 50, 270, 40, 20, juce::Justification::centred);
            g.drawText("Q " + juce::String(b+1), bx + 100, 270, 40, 20, juce::Justification::centred);
        }
    }

    void resized() override {
        for (int b = 0; b < 3; ++b) {
            int bx = 20 + b * 150;
            slidersF[b]->setBounds(bx, 220, 40, 40);
            slidersG[b]->setBounds(bx + 50, 220, 40, 40);
            slidersQ[b]->setBounds(bx + 100, 220, 40, 40);
        }
    }

private:
    void setupSlider(juce::String name, std::atomic<float>* value, float min, float max, std::unique_ptr<juce::Slider>& ptr) {
        ptr = std::make_unique<juce::Slider>(juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox);
        ptr->setRange(min, max);
        if (name.startsWith("F")) ptr->setSkewFactorFromMidPoint(std::sqrt(min * max));
        ptr->setValue(value->load());
        ptr->setDoubleClickReturnValue(true, value->load());
        ptr->onValueChange = [ptr = ptr.get(), value, this]() {
            value->store((float)ptr->getValue(), std::memory_order_relaxed);
            repaint();
        };
        addAndMakeVisible(*ptr);
    }

    ParametricEQEffect* pEffect;
    juce::dsp::FFT forwardFFT;
    juce::dsp::WindowingFunction<float> window;
    
    static constexpr int spectrumSize = 256;
    std::array<float, spectrumSize> spectrumData;

    std::unique_ptr<juce::Slider> slidersF[3];
    std::unique_ptr<juce::Slider> slidersG[3];
    std::unique_ptr<juce::Slider> slidersQ[3];
};

// ─── ParametricEQEffect ──────────────────────────────────────────────────────
ParametricEQEffect::ParametricEQEffect() {
}

void ParametricEQEffect::prepareToPlay(double sampleRate) {
    currentSampleRate = sampleRate;
    juce::dsp::ProcessSpec spec;
    spec.sampleRate = sampleRate;
    spec.maximumBlockSize = 8192;
    spec.numChannels = 2;
    eqChain.prepare(spec);
}

void ParametricEQEffect::processBlock(juce::AudioBuffer<float>& buffer) {
    eqChain.template get<0>().coefficients = juce::dsp::IIR::Coefficients<float>::makePeakFilter(
        currentSampleRate, freq1.load(std::memory_order_relaxed), q1.load(std::memory_order_relaxed), gain1.load(std::memory_order_relaxed));

    eqChain.template get<1>().coefficients = juce::dsp::IIR::Coefficients<float>::makePeakFilter(
        currentSampleRate, freq2.load(std::memory_order_relaxed), q2.load(std::memory_order_relaxed), gain2.load(std::memory_order_relaxed));

    eqChain.template get<2>().coefficients = juce::dsp::IIR::Coefficients<float>::makePeakFilter(
        currentSampleRate, freq3.load(std::memory_order_relaxed), q3.load(std::memory_order_relaxed), gain3.load(std::memory_order_relaxed));

    juce::dsp::AudioBlock<float> block(buffer);
    juce::dsp::ProcessContextReplacing<float> context(block);
    eqChain.process(context);

    if (buffer.getNumChannels() > 0) {
        auto* channelData = buffer.getReadPointer(0);
        int numSamples = buffer.getNumSamples();
        int start1, size1, start2, size2;
        audioFifo.prepareToWrite(numSamples, start1, size1, start2, size2);
        if (size1 > 0) std::copy(channelData, channelData + size1, audioFifoData.begin() + start1);
        if (size2 > 0) std::copy(channelData + size1, channelData + size1 + size2, audioFifoData.begin() + start2);
        audioFifo.finishedWrite(size1 + size2);
    }
}

void ParametricEQEffect::clear() {
    eqChain.reset();
}

juce::ValueTree ParametricEQEffect::saveState() const {
    juce::ValueTree state("ParametricEQState");
    state.setProperty("freq1", freq1.load(), nullptr);
    state.setProperty("gain1", gain1.load(), nullptr);
    state.setProperty("q1", q1.load(), nullptr);
    
    state.setProperty("freq2", freq2.load(), nullptr);
    state.setProperty("gain2", gain2.load(), nullptr);
    state.setProperty("q2", q2.load(), nullptr);
    
    state.setProperty("freq3", freq3.load(), nullptr);
    state.setProperty("gain3", gain3.load(), nullptr);
    state.setProperty("q3", q3.load(), nullptr);
    
    return state;
}

void ParametricEQEffect::loadState(const juce::ValueTree& tree) {
    if (tree.hasProperty("freq1")) freq1.store(tree.getProperty("freq1"), std::memory_order_relaxed);
    if (tree.hasProperty("gain1")) gain1.store(tree.getProperty("gain1"), std::memory_order_relaxed);
    if (tree.hasProperty("q1")) q1.store(tree.getProperty("q1"), std::memory_order_relaxed);
    
    if (tree.hasProperty("freq2")) freq2.store(tree.getProperty("freq2"), std::memory_order_relaxed);
    if (tree.hasProperty("gain2")) gain2.store(tree.getProperty("gain2"), std::memory_order_relaxed);
    if (tree.hasProperty("q2")) q2.store(tree.getProperty("q2"), std::memory_order_relaxed);
    
    if (tree.hasProperty("freq3")) freq3.store(tree.getProperty("freq3"), std::memory_order_relaxed);
    if (tree.hasProperty("gain3")) gain3.store(tree.getProperty("gain3"), std::memory_order_relaxed);
    if (tree.hasProperty("q3")) q3.store(tree.getProperty("q3"), std::memory_order_relaxed);
}

std::unique_ptr<juce::Component> ParametricEQEffect::createEditor() {
    return std::make_unique<ParametricEQEditor>(this);
}

