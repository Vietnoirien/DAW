#include "BuiltInEffects.h"
#include "../UI/LiBeLookAndFeel.h"
#include "BespokeEffectEditors.h"

// ─── Shared editor constants ──────────────────────────────────────────────────
static constexpr int kEditorFftOrder = 10;
static constexpr int kEditorFftSize  = 1 << kEditorFftOrder; // 1024
static constexpr int kEditorSpecSize = 200;

// Helper: push post-processed audio into an effect's FIFO (call from processBlock)
template<int FifoCapacity>
static void pushToFifo(juce::AbstractFifo& fifo,
                       std::array<float, FifoCapacity>& fifoData,
                       const float* channelData, int numSamples)
{
    int s1, n1, s2, n2;
    fifo.prepareToWrite(numSamples, s1, n1, s2, n2);
    if (n1 > 0) std::copy(channelData, channelData + n1, fifoData.begin() + s1);
    if (n2 > 0) std::copy(channelData + n1, channelData + n1 + n2, fifoData.begin() + s2);
    fifo.finishedWrite(n1 + n2);
}

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

    if (buffer.getNumChannels() > 0)
        pushToFifo<4096>(audioFifo, audioFifoData,
                         buffer.getReadPointer(0), buffer.getNumSamples());
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

void ReverbEffect::registerAutomationParameters(AutomationRegistry* registry) {
    if (!registry) return;
    registry->registerParameter("Reverb/Size", &roomSize);
    registry->registerParameter("Reverb/Damping", &damping);
    registry->registerParameter("Reverb/Wet Level", &wetLevel);
    registry->registerParameter("Reverb/Dry Level", &dryLevel);
    registry->registerParameter("Reverb/Width", &width);
}

std::unique_ptr<juce::Component> ReverbEffect::createEditor() {
    return std::make_unique<ReverbEditor>(this);
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

    if (buffer.getNumChannels() > 0)
        pushToFifo<4096>(audioFifo, audioFifoData,
                         buffer.getReadPointer(0), buffer.getNumSamples());
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

void DelayEffect::registerAutomationParameters(AutomationRegistry* registry) {
    if (!registry) return;
    registry->registerParameter("Delay/Time",     &delayTimeMs, 10.0f, 1000.0f);
    registry->registerParameter("Delay/Feedback", &feedback,     0.0f,    0.95f);
    registry->registerParameter("Delay/Mix",      &mix,          0.0f,    1.0f);
}

std::unique_ptr<juce::Component> DelayEffect::createEditor() {
    return std::make_unique<DelayEditor>(this);
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

    if (buffer.getNumChannels() > 0)
        pushToFifo<4096>(audioFifo, audioFifoData,
                         buffer.getReadPointer(0), buffer.getNumSamples());
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

void ChorusEffect::registerAutomationParameters(AutomationRegistry* registry) {
    if (!registry) return;
    registry->registerParameter("Chorus/Rate",     &rate,        0.0f,   99.0f);
    registry->registerParameter("Chorus/Depth",    &depth,       0.0f,    1.0f);
    registry->registerParameter("Chorus/Delay",    &centreDelay, 1.0f,  100.0f);
    registry->registerParameter("Chorus/Feedback", &feedback,   -1.0f,    1.0f);
    registry->registerParameter("Chorus/Mix",      &mix,         0.0f,    1.0f);
}

std::unique_ptr<juce::Component> ChorusEffect::createEditor() {
    return std::make_unique<ChorusEditor>(this);
}

// ─── FilterEffect ────────────────────────────────────────────────────────────
FilterEffect::FilterEffect() {
    filter.setType(juce::dsp::StateVariableTPTFilterType::lowpass);
}

void FilterEffect::prepareToPlay(double sampleRate) {
    currentSampleRate = sampleRate;
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

    if (buffer.getNumChannels() > 0)
        pushToFifo<4096>(audioFifo, audioFifoData,
                         buffer.getReadPointer(0), buffer.getNumSamples());
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

void FilterEffect::registerAutomationParameters(AutomationRegistry* registry) {
    if (!registry) return;
    registry->registerParameter("Filter/Cutoff",    &cutoff,    20.0f, 20000.0f);
    registry->registerParameter("Filter/Resonance", &resonance,  0.1f,    10.0f);
}

std::unique_ptr<juce::Component> FilterEffect::createEditor() {
    return std::make_unique<FilterEditor>(this);
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
    // Measure input RMS before compression
    float sumSq = 0.0f;
    int total = buffer.getNumChannels() * buffer.getNumSamples();
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch) {
        auto* d = buffer.getReadPointer(ch);
        for (int i = 0; i < buffer.getNumSamples(); ++i) sumSq += d[i] * d[i];
    }
    float rms = (total > 0) ? std::sqrt(sumSq / total) : 0.0f;
    float inDb = juce::Decibels::gainToDecibels(rms, -100.0f);
    inputLevelDb.store(inDb, std::memory_order_relaxed);

    compressor.setThreshold(threshold.load(std::memory_order_relaxed));
    compressor.setRatio(ratio.load(std::memory_order_relaxed));
    compressor.setAttack(attack.load(std::memory_order_relaxed));
    compressor.setRelease(release.load(std::memory_order_relaxed));

    juce::dsp::AudioBlock<float> block(buffer);
    juce::dsp::ProcessContextReplacing<float> context(block);
    compressor.process(context);

    // Compute gain reduction analytically from input level
    float thr = threshold.load(std::memory_order_relaxed);
    float rat = ratio.load(std::memory_order_relaxed);
    float gr  = (inDb > thr && rat > 1.0f)
                ? (inDb - thr) * (1.0f - 1.0f / rat)
                : 0.0f;
    gainReductionDb.store(gr, std::memory_order_relaxed);
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

void CompressorEffect::registerAutomationParameters(AutomationRegistry* registry) {
    if (!registry) return;
    registry->registerParameter("Compressor/Threshold", &threshold, -60.0f,    0.0f);
    registry->registerParameter("Compressor/Ratio",     &ratio,       1.0f,   20.0f);
    registry->registerParameter("Compressor/Attack",    &attack,      0.1f,  100.0f);
    registry->registerParameter("Compressor/Release",   &release,    10.0f, 1000.0f);
}

std::unique_ptr<juce::Component> CompressorEffect::createEditor() {
    return std::make_unique<CompressorEditor>(this);
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
    // Measure input peak before limiting
    float inPeak = 0.0f;
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch) {
        auto* d = buffer.getReadPointer(ch);
        for (int i = 0; i < buffer.getNumSamples(); ++i)
            inPeak = std::max(inPeak, std::abs(d[i]));
    }
    inputLevelDb.store(juce::Decibels::gainToDecibels(inPeak, -100.0f), std::memory_order_relaxed);

    float thr = threshold.load(std::memory_order_relaxed);
    limiter.setThreshold(thr);
    limiter.setRelease(release.load(std::memory_order_relaxed));

    juce::dsp::AudioBlock<float> block(buffer);
    juce::dsp::ProcessContextReplacing<float> context(block);
    limiter.process(context);

    // ── Makeup-gain correction ────────────────────────────────────────────
    // juce::dsp::Limiter applies automatic makeup gain = decibelsToGain(-thr)
    // so it behaves as a "maximiser" (signals below threshold are boosted).
    // We cancel that by applying the inverse so the net effect is brickwall
    // clamping at `thr` dBFS with no gain change below threshold.
    buffer.applyGain(juce::Decibels::decibelsToGain(thr));

    // Measure output peak after limiting
    float outPeak = 0.0f;
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch) {
        auto* d = buffer.getReadPointer(ch);
        for (int i = 0; i < buffer.getNumSamples(); ++i)
            outPeak = std::max(outPeak, std::abs(d[i]));
    }
    outputLevelDb.store(juce::Decibels::gainToDecibels(outPeak, -100.0f), std::memory_order_relaxed);
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

void LimiterEffect::registerAutomationParameters(AutomationRegistry* registry) {
    if (!registry) return;
    registry->registerParameter("Limiter/Threshold", &threshold, -40.0f,    0.0f);
    registry->registerParameter("Limiter/Release",   &release,   10.0f, 1000.0f);
}

std::unique_ptr<juce::Component> LimiterEffect::createEditor() {
    return std::make_unique<LimiterEditor>(this);
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

    if (buffer.getNumChannels() > 0)
        pushToFifo<4096>(audioFifo, audioFifoData,
                         buffer.getReadPointer(0), buffer.getNumSamples());
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

void PhaserEffect::registerAutomationParameters(AutomationRegistry* registry) {
    if (!registry) return;
    registry->registerParameter("Phaser/Rate",      &rate,       0.01f,  10.0f);
    registry->registerParameter("Phaser/Depth",     &depth,       0.0f,   1.0f);
    registry->registerParameter("Phaser/Frequency", &centreFreq, 50.0f, 5000.0f);
    registry->registerParameter("Phaser/Feedback",  &feedback,   -1.0f,   1.0f);
    registry->registerParameter("Phaser/Mix",       &mix,         0.0f,   1.0f);
}

std::unique_ptr<juce::Component> PhaserEffect::createEditor() {
    return std::make_unique<PhaserEditor>(this);
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

    if (buffer.getNumChannels() > 0)
        pushToFifo<4096>(audioFifo, audioFifoData,
                         buffer.getReadPointer(0), buffer.getNumSamples());
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

void SaturationEffect::registerAutomationParameters(AutomationRegistry* registry) {
    if (!registry) return;
    registry->registerParameter("Saturation/Drive", &drive, 1.0f, 20.0f);
    registry->registerParameter("Saturation/Mix",   &mix,   0.0f,  1.0f);
}

std::unique_ptr<juce::Component> SaturationEffect::createEditor() {
    return std::make_unique<SaturationEditor>(this);
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

        setupSlider("F1 Hz", &pEffect->freq1, 20.0f, 500.0f, slidersF[0], "EQ/Band 1/Freq");
        setupSlider("G1", &pEffect->gain1, 0.1f, 10.0f, slidersG[0], "EQ/Band 1/Gain");
        setupSlider("Q1", &pEffect->q1, 0.1f, 10.0f, slidersQ[0], "EQ/Band 1/Q");

        setupSlider("F2 Hz", &pEffect->freq2, 500.0f, 5000.0f, slidersF[1], "EQ/Band 2/Freq");
        setupSlider("G2", &pEffect->gain2, 0.1f, 10.0f, slidersG[1], "EQ/Band 2/Gain");
        setupSlider("Q2", &pEffect->q2, 0.1f, 10.0f, slidersQ[1], "EQ/Band 2/Q");

        setupSlider("F3 Hz", &pEffect->freq3, 5000.0f, 20000.0f, slidersF[2], "EQ/Band 3/Freq");
        setupSlider("G3", &pEffect->gain3, 0.1f, 10.0f, slidersG[2], "EQ/Band 3/Gain");
        setupSlider("Q3", &pEffect->q3, 0.1f, 10.0f, slidersQ[2], "EQ/Band 3/Q");

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

            auto drawInd = [&](juce::Slider* s) {
                if (s->getProperties().contains("isAutomated") && (bool)s->getProperties()["isAutomated"]) {
                    g.setColour(juce::Colours::orange);
                    g.drawEllipse(s->getBounds().toFloat().reduced(2.0f), 2.0f);
                }
            };
            drawInd(slidersF[b].get());
            drawInd(slidersG[b].get());
            drawInd(slidersQ[b].get());
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
    void setupSlider(juce::String name, std::atomic<float>* value, float min, float max, std::unique_ptr<juce::Slider>& ptr, juce::String paramId) {
        ptr = std::make_unique<juce::Slider>(juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox);
        ptr->setRange(min, max);
        if (name.startsWith("F")) ptr->setSkewFactorFromMidPoint(std::sqrt(min * max));
        ptr->setValue(value->load());
        ptr->setDoubleClickReturnValue(true, value->load());
        ptr->getProperties().set("parameterId", paramId);
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

void ParametricEQEffect::registerAutomationParameters(AutomationRegistry* registry) {
    if (!registry) return;
    // Ranges match ParametricEQEditor slider ranges exactly
    registry->registerParameter("EQ/Band 1/Freq",  &freq1,   20.0f,    500.0f);
    registry->registerParameter("EQ/Band 1/Gain",  &gain1,    0.1f,     10.0f);
    registry->registerParameter("EQ/Band 1/Q",     &q1,       0.1f,     10.0f);

    registry->registerParameter("EQ/Band 2/Freq",  &freq2,  500.0f,   5000.0f);
    registry->registerParameter("EQ/Band 2/Gain",  &gain2,    0.1f,     10.0f);
    registry->registerParameter("EQ/Band 2/Q",     &q2,       0.1f,     10.0f);

    registry->registerParameter("EQ/Band 3/Freq",  &freq3, 5000.0f,  20000.0f);
    registry->registerParameter("EQ/Band 3/Gain",  &gain3,    0.1f,     10.0f);
    registry->registerParameter("EQ/Band 3/Q",     &q3,       0.1f,     10.0f);
}

std::unique_ptr<juce::Component> ParametricEQEffect::createEditor() {
    return std::make_unique<ParametricEQEditor>(this);
}

// ─── GainEffect ──────────────────────────────────────────────────────────────
GainEffect::GainEffect() {}

void GainEffect::prepareToPlay(double sampleRate) {
    juce::dsp::ProcessSpec spec;
    spec.sampleRate = sampleRate;
    spec.maximumBlockSize = 8192;
    spec.numChannels = 2;
    gainProc.prepare(spec);
}

void GainEffect::processBlock(juce::AudioBuffer<float>& buffer) {
    // Measure input peak
    float inPeak = 0.0f;
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch) {
        auto* d = buffer.getReadPointer(ch);
        for (int i = 0; i < buffer.getNumSamples(); ++i)
            inPeak = std::max(inPeak, std::abs(d[i]));
    }
    inputLevelDb.store(juce::Decibels::gainToDecibels(inPeak, -100.0f), std::memory_order_relaxed);

    // Apply gain
    gainProc.setGainDecibels(gainDb.load(std::memory_order_relaxed));
    juce::dsp::AudioBlock<float> block(buffer);
    juce::dsp::ProcessContextReplacing<float> ctx(block);
    gainProc.process(ctx);

    // Apply pan (cosine law)
    float p = pan.load(std::memory_order_relaxed); // -1..+1
    if (buffer.getNumChannels() >= 2) {
        float leftGain  = std::cos((p + 1.0f) * juce::MathConstants<float>::pi * 0.25f);
        float rightGain = std::sin((p + 1.0f) * juce::MathConstants<float>::pi * 0.25f);
        buffer.applyGain(0, 0, buffer.getNumSamples(), leftGain);
        buffer.applyGain(1, 0, buffer.getNumSamples(), rightGain);
    }

    // Measure output peak
    float outPeak = 0.0f;
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch) {
        auto* d = buffer.getReadPointer(ch);
        for (int i = 0; i < buffer.getNumSamples(); ++i)
            outPeak = std::max(outPeak, std::abs(d[i]));
    }
    outputLevelDb.store(juce::Decibels::gainToDecibels(outPeak, -100.0f), std::memory_order_relaxed);
}

void GainEffect::clear() { gainProc.reset(); }

juce::ValueTree GainEffect::saveState() const {
    juce::ValueTree state("GainState");
    state.setProperty("gainDb", gainDb.load(), nullptr);
    state.setProperty("pan",    pan.load(),    nullptr);
    return state;
}

void GainEffect::loadState(const juce::ValueTree& tree) {
    if (tree.hasProperty("gainDb")) gainDb.store(tree.getProperty("gainDb"), std::memory_order_relaxed);
    if (tree.hasProperty("pan"))    pan.store(tree.getProperty("pan"),    std::memory_order_relaxed);
}

void GainEffect::registerAutomationParameters(AutomationRegistry* registry) {
    if (!registry) return;
    registry->registerParameter("Gain/Gain dB", &gainDb, -40.0f, 40.0f);
    registry->registerParameter("Gain/Pan",     &pan,    -1.0f,   1.0f);
}

// ─── GainEditor ──────────────────────────────────────────────────────────────
class GainEditor : public BespokeEffectEditor {
public:
    GainEditor(GainEffect* fx) : BespokeEffectEditor("Gain", juce::Colour(0xffA8EDEA)), effect(fx) {
        setupKnob(kGain, "Gain dB", &effect->gainDb, -40.0f, 40.0f, "Gain/Gain dB");
        setupKnob(kPan,  "Pan",     &effect->pan,    -1.0f,  1.0f,  "Gain/Pan");
        setSize(20 + 2 * (LiBeKnob::kW + 10) - 10, 220);
    }

    void timerCallback() override {
        smoothIn  = std::max(effect->inputLevelDb.load(),  smoothIn  - 0.8f);
        smoothOut = std::max(effect->outputLevelDb.load(), smoothOut - 0.8f);
        repaint();
    }

    void drawVisualiser(juce::Graphics& g, juce::Rectangle<int> b) override {
        // Two vertical bar meters: IN (left) and OUT (right)
        auto drawMeterBar = [&](juce::Rectangle<int> r, float db) {
            g.setColour(juce::Colour(0xff1A1A2A));
            g.fillRect(r);
            float norm = juce::jlimit(0.0f, 1.0f, juce::jmap(db, -60.0f, 12.0f, 0.0f, 1.0f));
            int fillH = int(r.getHeight() * norm);
            auto fill = r.removeFromBottom(fillH);
            // Colour gradient: green → yellow → red
            juce::ColourGradient grad(juce::Colour(0xff4CAF50), fill.getBottomLeft().toFloat(),
                                      juce::Colour(0xffFF5252), fill.getTopLeft().toFloat(), false);
            grad.addColour(0.75, juce::Colour(0xffFFD54F));
            g.setGradientFill(grad);
            g.fillRect(fill);
            g.setColour(juce::Colour(0xff2A2A3A));
            g.drawRect(r.expanded(0, fillH), 1);
        };

        int mW = (b.getWidth() - 30) / 2;
        drawMeterBar({ b.getX() + 5,       b.getY() + 5, mW, b.getHeight() - 10 }, smoothIn);
        drawMeterBar({ b.getX() + mW + 20, b.getY() + 5, mW, b.getHeight() - 10 }, smoothOut);

        g.setColour(juce::Colours::white.withAlpha(0.6f));
        g.setFont(9.0f);
        g.drawText("IN",  b.getX() + 5,         b.getBottom() - 14, mW, 12, juce::Justification::centred);
        g.drawText("OUT", b.getX() + mW + 20,   b.getBottom() - 14, mW, 12, juce::Justification::centred);

        // 0 dB line
        float y0 = b.getY() + 5 + (b.getHeight() - 10) * (1.0f - juce::jmap(0.0f, -60.0f, 12.0f, 0.0f, 1.0f));
        g.setColour(juce::Colours::white.withAlpha(0.4f));
        g.drawHorizontalLine(int(y0), float(b.getX()), float(b.getRight()));
        g.drawText("0",  b.getRight() - 18, int(y0) - 5, 16, 10, juce::Justification::centred);
    }

private:
    GainEffect* effect;
    float smoothIn = -100.0f, smoothOut = -100.0f;
    LiBeKnob kGain, kPan;
    void setupKnob(LiBeKnob& k, const juce::String& lbl, std::atomic<float>* ptr,
                   float min, float max, const juce::String& id) {
        k.setup(lbl, ptr->load(), min, max, ptr->load(), id, accent);
        k.slider.onValueChange = [&k, ptr]() {
            ptr->store((float)k.slider.getValue(), std::memory_order_relaxed);
        };
        k.startTrackingParam(ptr, min, max);
        addKnob(&k);
    }
};

std::unique_ptr<juce::Component> GainEffect::createEditor() {
    return std::make_unique<GainEditor>(this);
}

// ─── TransientShaperEffect ───────────────────────────────────────────────────
TransientShaperEffect::TransientShaperEffect() {}

void TransientShaperEffect::prepareToPlay(double sampleRate) {
    currentSampleRate = sampleRate;
    // Fast follower: 1ms attack
    alphaFast = std::exp(-1.0f / float(sampleRate * 0.001));
    // Slow follower: 15ms attack
    alphaSlow = std::exp(-1.0f / float(sampleRate * 0.015));
    fastEnv.fill(0.0f);
    slowEnv.fill(0.0f);
}

void TransientShaperEffect::processBlock(juce::AudioBuffer<float>& buffer) {
    const int numCh   = buffer.getNumChannels();
    const int numSamp = buffer.getNumSamples();
    const float atk  = attackAmt.load(std::memory_order_relaxed);
    const float sus  = sustainAmt.load(std::memory_order_relaxed);
    const float sens = sensitivity.load(std::memory_order_relaxed);

    for (int ch = 0; ch < std::min(numCh, 2); ++ch) {
        float* data = buffer.getWritePointer(ch);
        for (int i = 0; i < numSamp; ++i) {
            float x    = std::abs(data[i]);
            float fast = alphaFast * fastEnv[ch] + (1.0f - alphaFast) * x;
            float slow = alphaSlow * slowEnv[ch] + (1.0f - alphaSlow) * x;
            fastEnv[ch] = fast;
            slowEnv[ch] = slow;

            // Differential: positive when a transient is detected
            float diff = (fast - slow) * sens;

            // Gain modulation
            float mod = 1.0f + atk * diff + sus * slow;
            mod = juce::jlimit(0.0f, 4.0f, mod);
            data[i] *= mod;
        }
    }

    // Expose last-sample envelope for the editor
    fastEnvDisplay.store(fastEnv[0], std::memory_order_relaxed);
    slowEnvDisplay.store(slowEnv[0], std::memory_order_relaxed);

    if (buffer.getNumChannels() > 0)
        pushToFifo<4096>(audioFifo, audioFifoData, buffer.getReadPointer(0), numSamp);
}

void TransientShaperEffect::clear() {
    fastEnv.fill(0.0f);
    slowEnv.fill(0.0f);
}

juce::ValueTree TransientShaperEffect::saveState() const {
    juce::ValueTree state("TransientShaperState");
    state.setProperty("attack",    attackAmt.load(),  nullptr);
    state.setProperty("sustain",   sustainAmt.load(), nullptr);
    state.setProperty("sensitivity", sensitivity.load(), nullptr);
    return state;
}

void TransientShaperEffect::loadState(const juce::ValueTree& tree) {
    if (tree.hasProperty("attack"))     attackAmt.store(tree.getProperty("attack"),    std::memory_order_relaxed);
    if (tree.hasProperty("sustain"))    sustainAmt.store(tree.getProperty("sustain"),  std::memory_order_relaxed);
    if (tree.hasProperty("sensitivity")) sensitivity.store(tree.getProperty("sensitivity"), std::memory_order_relaxed);
}

void TransientShaperEffect::registerAutomationParameters(AutomationRegistry* registry) {
    if (!registry) return;
    registry->registerParameter("TransientShaper/Attack",      &attackAmt,   -1.0f, 1.0f);
    registry->registerParameter("TransientShaper/Sustain",     &sustainAmt,  -1.0f, 1.0f);
    registry->registerParameter("TransientShaper/Sensitivity", &sensitivity,  0.1f, 10.0f);
}

// ─── TransientShaperEditor ───────────────────────────────────────────────────
class TransientShaperEditor : public BespokeEffectEditor {
public:
    TransientShaperEditor(TransientShaperEffect* fx)
        : BespokeEffectEditor("Transient Shaper", juce::Colour(0xffFFA040)), effect(fx) {
        smoothSpectrum.fill(-100.0f);
        setupKnob(kAttack,    "Attack",  &effect->attackAmt,  -1.0f, 1.0f, "TransientShaper/Attack");
        setupKnob(kSustain,   "Sustain", &effect->sustainAmt, -1.0f, 1.0f, "TransientShaper/Sustain");
        setupKnob(kSensitivity,"Sens",   &effect->sensitivity, 0.1f, 10.0f, "TransientShaper/Sensitivity");
        setSize(20 + 3 * (LiBeKnob::kW + 10) - 10, 220);
    }

    void timerCallback() override {
        updateSpectrum(effect->audioFifo, effect->audioFifoData, 44100.0);
        fastVal = fastVal * 0.7f + effect->fastEnvDisplay.load() * 0.3f;
        slowVal = slowVal * 0.7f + effect->slowEnvDisplay.load() * 0.3f;
        repaint();
    }

    void drawVisualiser(juce::Graphics& g, juce::Rectangle<int> b) override {
        drawSpectrum(g, b, accent);

        // Draw two envelope curves on top: fast (accent) and slow (white)
        auto drawEnv = [&](float envVal, juce::Colour col) {
            float norm = juce::jlimit(0.0f, 1.0f, envVal * 4.0f); // scale for display
            juce::Path p;
            for (int x = 0; x < b.getWidth(); ++x) {
                float phase = float(x) / b.getWidth();
                // Visualise as a decaying tail — shows "snapshot" of envelope level
                float y = b.getBottom() - norm * b.getHeight() * std::exp(-phase * 3.0f);
                if (x == 0) p.startNewSubPath(b.getX() + x, y);
                else p.lineTo(b.getX() + x, y);
            }
            g.setColour(col);
            g.strokePath(p, juce::PathStrokeType(2.0f, juce::PathStrokeType::curved));
        };
        drawEnv(slowVal, juce::Colours::white.withAlpha(0.5f));
        drawEnv(fastVal, accent);

        // Label
        g.setColour(juce::Colours::white.withAlpha(0.5f));
        g.setFont(9.0f);
        g.drawText("FAST", b.getX() + 4, b.getY() + 4, 30, 10, juce::Justification::left);
        g.setColour(accent.withAlpha(0.8f));
        g.drawText("SLOW", b.getX() + 4, b.getY() + 16, 30, 10, juce::Justification::left);
    }

private:
    TransientShaperEffect* effect;
    float fastVal = 0.0f, slowVal = 0.0f;
    LiBeKnob kAttack, kSustain, kSensitivity;
    void setupKnob(LiBeKnob& k, const juce::String& lbl, std::atomic<float>* ptr,
                   float min, float max, const juce::String& id) {
        k.setup(lbl, ptr->load(), min, max, ptr->load(), id, accent);
        k.slider.onValueChange = [&k, ptr]() {
            ptr->store((float)k.slider.getValue(), std::memory_order_relaxed);
        };
        k.startTrackingParam(ptr, min, max);
        addKnob(&k);
    }
};

std::unique_ptr<juce::Component> TransientShaperEffect::createEditor() {
    return std::make_unique<TransientShaperEditor>(this);
}

// ─── NoiseGateEffect ─────────────────────────────────────────────────────────
NoiseGateEffect::NoiseGateEffect() {}

void NoiseGateEffect::prepareToPlay(double sampleRate) {
    currentSampleRate = sampleRate;
    auto msToCoeff = [&](float ms) {
        return std::exp(-1.0f / float(sampleRate * ms * 0.001));
    };
    alphaAttack  = msToCoeff(attackMs.load());
    alphaRelease = msToCoeff(releaseMs.load());
    envGain = 0.0f;
    holdCounter = 0;
}

void NoiseGateEffect::processBlock(juce::AudioBuffer<float>& buffer) {
    const int numSamp = buffer.getNumSamples();
    const int numCh   = buffer.getNumChannels();
    const float thr   = juce::Decibels::decibelsToGain(threshold.load(std::memory_order_relaxed));
    const float range = juce::Decibels::decibelsToGain(rangeDb.load(std::memory_order_relaxed));
    const int   hold  = int(holdMs.load(std::memory_order_relaxed) * 0.001f * currentSampleRate);

    // Recompute smoothing coefficients from current parameters
    auto msToCoeff = [&](float ms) {
        return std::exp(-1.0f / float(currentSampleRate * ms * 0.001f));
    };
    alphaAttack  = msToCoeff(attackMs.load(std::memory_order_relaxed));
    alphaRelease = msToCoeff(releaseMs.load(std::memory_order_relaxed));

    for (int i = 0; i < numSamp; ++i) {
        // Compute peak across all channels
        float peak = 0.0f;
        for (int ch = 0; ch < numCh; ++ch)
            peak = std::max(peak, std::abs(buffer.getReadPointer(ch)[i]));

        bool aboveThr = (peak >= thr);
        float targetGain;
        if (aboveThr) {
            holdCounter = hold;
            targetGain  = 1.0f;
        } else if (holdCounter > 0) {
            --holdCounter;
            targetGain = 1.0f;
        } else {
            targetGain = range;
        }

        // Smooth with attack/release
        float alpha = (targetGain > envGain) ? (1.0f - alphaAttack) : (1.0f - alphaRelease);
        envGain += alpha * (targetGain - envGain);

        for (int ch = 0; ch < numCh; ++ch)
            buffer.getWritePointer(ch)[i] *= envGain;
    }

    gateOpen.store(envGain > 0.5f, std::memory_order_relaxed);
    gainReduction.store(envGain, std::memory_order_relaxed);

    if (buffer.getNumChannels() > 0)
        pushToFifo<4096>(audioFifo, audioFifoData, buffer.getReadPointer(0), numSamp);
}

void NoiseGateEffect::clear() {
    envGain = 0.0f;
    holdCounter = 0;
}

juce::ValueTree NoiseGateEffect::saveState() const {
    juce::ValueTree state("NoiseGateState");
    state.setProperty("threshold", threshold.load(), nullptr);
    state.setProperty("attackMs",  attackMs.load(),  nullptr);
    state.setProperty("releaseMs", releaseMs.load(), nullptr);
    state.setProperty("holdMs",    holdMs.load(),    nullptr);
    state.setProperty("rangeDb",   rangeDb.load(),   nullptr);
    return state;
}

void NoiseGateEffect::loadState(const juce::ValueTree& tree) {
    if (tree.hasProperty("threshold")) threshold.store(tree.getProperty("threshold"), std::memory_order_relaxed);
    if (tree.hasProperty("attackMs"))  attackMs.store(tree.getProperty("attackMs"),   std::memory_order_relaxed);
    if (tree.hasProperty("releaseMs")) releaseMs.store(tree.getProperty("releaseMs"), std::memory_order_relaxed);
    if (tree.hasProperty("holdMs"))    holdMs.store(tree.getProperty("holdMs"),       std::memory_order_relaxed);
    if (tree.hasProperty("rangeDb"))   rangeDb.store(tree.getProperty("rangeDb"),     std::memory_order_relaxed);
}

void NoiseGateEffect::registerAutomationParameters(AutomationRegistry* registry) {
    if (!registry) return;
    registry->registerParameter("NoiseGate/Threshold", &threshold, -80.0f,    0.0f);
    registry->registerParameter("NoiseGate/Attack",    &attackMs,   0.1f,   100.0f);
    registry->registerParameter("NoiseGate/Release",   &releaseMs, 10.0f,  2000.0f);
    registry->registerParameter("NoiseGate/Hold",      &holdMs,     0.0f,   500.0f);
    registry->registerParameter("NoiseGate/Range",     &rangeDb,  -80.0f,    0.0f);
}

// ─── NoiseGateEditor ─────────────────────────────────────────────────────────
class NoiseGateEditor : public BespokeEffectEditor {
public:
    NoiseGateEditor(NoiseGateEffect* fx)
        : BespokeEffectEditor("Noise Gate", juce::Colour(0xff69F0AE)), effect(fx) {
        smoothSpectrum.fill(-100.0f);
        setupKnob(kThresh,  "Thresh",  &effect->threshold, -80.0f,   0.0f, "NoiseGate/Threshold");
        setupKnob(kAttack,  "Attack",  &effect->attackMs,   0.1f, 100.0f,  "NoiseGate/Attack");
        setupKnob(kRelease, "Release", &effect->releaseMs, 10.0f, 2000.0f, "NoiseGate/Release");
        setupKnob(kHold,    "Hold",    &effect->holdMs,     0.0f,  500.0f, "NoiseGate/Hold");
        setupKnob(kRange,   "Range",   &effect->rangeDb,  -80.0f,    0.0f, "NoiseGate/Range");
        setSize(20 + 5 * (LiBeKnob::kW + 10) - 10, 220);
    }

    void timerCallback() override {
        updateSpectrum(effect->audioFifo, effect->audioFifoData, 44100.0);
        isOpen = effect->gateOpen.load();
        smoothGr = smoothGr * 0.8f + effect->gainReduction.load() * 0.2f;
        repaint();
    }

    void drawVisualiser(juce::Graphics& g, juce::Rectangle<int> b) override {
        drawSpectrum(g, b, accent);

        // Threshold line
        float thrNorm = juce::jmap(effect->threshold.load(), -80.0f, 0.0f, 0.0f, 1.0f);
        thrNorm = juce::jlimit(0.0f, 1.0f, thrNorm);
        float thrX = b.getX() + b.getWidth() * thrNorm;  // map to x-axis (not frequency for clarity)
        float thrY = b.getY() + b.getHeight() * (1.0f - thrNorm);
        g.setColour(juce::Colours::white.withAlpha(0.6f));
        g.drawHorizontalLine(int(thrY), float(b.getX()), float(b.getRight()));
        g.drawText("THR", b.getX() + 4, int(thrY) - 11, 28, 10, juce::Justification::left);

        // OPEN / CLOSED LED
        float ledSize = 16.0f;
        juce::Rectangle<float> led(b.getRight() - ledSize - 6, b.getY() + 6, ledSize, ledSize);
        g.setColour(isOpen ? accent : juce::Colour(0xff333333));
        g.fillEllipse(led);
        g.setColour(isOpen ? accent.brighter(0.4f) : juce::Colour(0xff555555));
        g.drawEllipse(led, 1.5f);
        g.setColour(juce::Colours::white.withAlpha(0.7f));
        g.setFont(8.0f);
        g.drawText(isOpen ? "OPEN" : "GATE", b.getRight() - 40, b.getY() + 24, 38, 10, juce::Justification::centred);
    }

private:
    NoiseGateEffect* effect;
    bool  isOpen  = false;
    float smoothGr = 0.0f;
    LiBeKnob kThresh, kAttack, kRelease, kHold, kRange;
    void setupKnob(LiBeKnob& k, const juce::String& lbl, std::atomic<float>* ptr,
                   float min, float max, const juce::String& id) {
        k.setup(lbl, ptr->load(), min, max, ptr->load(), id, accent);
        k.slider.onValueChange = [&k, ptr]() {
            ptr->store((float)k.slider.getValue(), std::memory_order_relaxed);
        };
        k.startTrackingParam(ptr, min, max);
        addKnob(&k);
    }
};

std::unique_ptr<juce::Component> NoiseGateEffect::createEditor() {
    return std::make_unique<NoiseGateEditor>(this);
}
