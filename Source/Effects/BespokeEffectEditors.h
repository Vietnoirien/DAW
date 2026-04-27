#pragma once

#include <JuceHeader.h>
#include "BuiltInEffects.h"
#include "../UI/LiBeLookAndFeel.h"

// ─── Base Editor ─────────────────────────────────────────────────────────────
class BespokeEffectEditor : public juce::Component, public juce::Timer {
public:
    BespokeEffectEditor(const juce::String& name, juce::Colour acc)
        : effectName(name), accent(acc) {
        laf.defaultAccent = accent;
        setLookAndFeel(&laf);
        startTimerHz(30);
    }
    
    ~BespokeEffectEditor() override {
        setLookAndFeel(nullptr);
    }

    void paint(juce::Graphics& g) override {
        // Background
        g.fillAll(juce::Colour(0xff0D0D14));
        g.setColour(juce::Colour(0xff2A2A3A));
        g.drawRect(getLocalBounds(), 1);

        // Header
        juce::Rectangle<int> headerRect(0, 0, getWidth(), 28);
        g.setColour(juce::Colour(0xff141420));
        g.fillRect(headerRect);
        g.setColour(accent);
        g.setFont(juce::Font(juce::FontOptions(13.0f, juce::Font::bold)));
        g.drawText(" " + effectName, headerRect, juce::Justification::centredLeft);

        // Visualiser area
        auto visBounds = getVisualiserBounds();
        g.setColour(juce::Colour(0xff111118));
        g.fillRect(visBounds);
        g.setColour(juce::Colour(0xff222230));
        g.drawRect(visBounds, 1);
        
        drawVisualiser(g, visBounds);
    }

    void resized() override {
        int x = 10;
        int y = getVisualiserBounds().getBottom() + 15;
        for (auto* k : knobs) {
            k->setBounds(x, y, LiBeKnob::kW, LiBeKnob::kH);
            x += LiBeKnob::kW + 10;
        }
    }

    virtual void drawVisualiser(juce::Graphics& g, juce::Rectangle<int> bounds) = 0;

    void addKnob(LiBeKnob* knob) {
        addAndMakeVisible(knob);
        knobs.add(knob);
    }

    virtual juce::Rectangle<int> getVisualiserBounds() const {
        return { 10, 38, getWidth() - 20, getHeight() - 38 - LiBeKnob::kH - 25 };
    }

protected:
    juce::String effectName;
    juce::Colour accent;
    LiBeLookAndFeel laf;
    juce::Array<LiBeKnob*> knobs;
    
    juce::dsp::FFT forwardFFT { 10 };
    juce::dsp::WindowingFunction<float> window { 1024, juce::dsp::WindowingFunction<float>::hann };
    std::array<float, 1024> spectrumData;
    std::array<float, 200> smoothSpectrum;

    void updateSpectrum(juce::AbstractFifo& fifo, std::array<float, 4096>& fifoData, double sampleRate) {
        if (fifo.getNumReady() >= 1024) {
            std::array<float, 1024> fftData;
            int start1, size1, start2, size2;
            fifo.prepareToRead(1024, start1, size1, start2, size2);
            if (size1 > 0) std::copy(fifoData.begin() + start1, fifoData.begin() + start1 + size1, fftData.begin());
            if (size2 > 0) std::copy(fifoData.begin() + start2, fifoData.begin() + start2 + size2, fftData.begin() + size1);
            fifo.finishedRead(size1 + size2);

            window.multiplyWithWindowingTable(fftData.data(), 1024);
            std::vector<float> fftBuffer(2048, 0.0f);
            std::copy(fftData.begin(), fftData.end(), fftBuffer.begin());
            forwardFFT.performFrequencyOnlyForwardTransform(fftBuffer.data());
            
            auto numBins = 512;
            for (int i = 0; i < 200; ++i) {
                float freq = juce::mapToLog10(float(i) / 200.0f, 20.0f, 20000.0f);
                float binIdx = freq * 1024.0f / (sampleRate > 0 ? sampleRate : 44100.0f);
                int bin = juce::jlimit(0, numBins - 1, (int)binIdx);
                float mag = fftBuffer[bin];
                float db = juce::Decibels::gainToDecibels(mag) - juce::Decibels::gainToDecibels(1024.0f);
                smoothSpectrum[i] = smoothSpectrum[i] * 0.7f + db * 0.3f;
            }
        }
    }
    
    void drawSpectrum(juce::Graphics& g, juce::Rectangle<int> bounds, juce::Colour col) {
        juce::Path p;
        for (int i = 0; i < 200; ++i) {
            float x = bounds.getX() + bounds.getWidth() * ((float)i / 200.0f);
            float y = juce::jmap(smoothSpectrum[i], -80.0f, 0.0f, (float)bounds.getBottom(), (float)bounds.getY());
            y = juce::jlimit((float)bounds.getY(), (float)bounds.getBottom(), y);
            if (i == 0) p.startNewSubPath(x, y);
            else p.lineTo(x, y);
        }
        if (!p.isEmpty()) {
            p.lineTo(bounds.getRight(), bounds.getBottom());
            p.lineTo(bounds.getX(), bounds.getBottom());
            p.closeSubPath();
            g.setColour(col.withAlpha(0.5f));
            g.fillPath(p);
        }
    }
};

// ─── Reverb Editor ───────────────────────────────────────────────────────────
class ReverbEditor : public BespokeEffectEditor {
public:
    ReverbEditor(ReverbEffect* fx) : BespokeEffectEditor("Reverb", juce::Colour(0xff7B68EE)), effect(fx) {
        smoothSpectrum.fill(-100.0f);
        setupKnob(kSize, "Size", &effect->roomSize, 0.0f, 1.0f, "Reverb/Size");
        setupKnob(kDamp, "Damp", &effect->damping, 0.0f, 1.0f, "Reverb/Damping");
        setupKnob(kMix, "Mix", &effect->wetLevel, 0.0f, 1.0f, "Reverb/Wet Level");
        setSize(20 + 3 * (LiBeKnob::kW + 10) - 10, 220);
    }
    
    void timerCallback() override {
        updateSpectrum(effect->audioFifo, effect->audioFifoData, 44100.0);
        repaint();
    }
    
    void drawVisualiser(juce::Graphics& g, juce::Rectangle<int> b) override {
        drawSpectrum(g, b, accent);
        // Decay curve
        float size = effect->roomSize.load();
        juce::Path p;
        p.startNewSubPath(b.getX(), b.getY() + 5);
        p.quadraticTo(b.getX() + b.getWidth() * size, b.getBottom(), b.getRight(), b.getBottom());
        g.setColour(juce::Colours::white.withAlpha(0.8f));
        g.strokePath(p, juce::PathStrokeType(2.0f));
    }
    
private:
    ReverbEffect* effect;
    LiBeKnob kSize, kDamp, kMix;
    void setupKnob(LiBeKnob& k, const juce::String& lbl, std::atomic<float>* ptr, float min, float max, const juce::String& id) {
        k.setup(lbl, ptr->load(), min, max, ptr->load(), id, accent);
        k.slider.onValueChange = [&k, ptr]() {
            ptr->store((float)k.slider.getValue(), std::memory_order_relaxed);
        };
        k.startTrackingParam(ptr, min, max);
        addKnob(&k);
    }
};

// ─── Delay Editor ────────────────────────────────────────────────────────────
class DelayEditor : public BespokeEffectEditor {
public:
    DelayEditor(DelayEffect* fx) : BespokeEffectEditor("Delay", juce::Colour(0xff00BCD4)), effect(fx) {
        smoothSpectrum.fill(-100.0f);
        setupKnob(kTime, "Time", &effect->delayTimeMs, 10.0f, 1000.0f, "Delay/Time");
        setupKnob(kFdbk, "Fdbk", &effect->feedback, 0.0f, 0.95f, "Delay/Feedback");
        setupKnob(kMix, "Mix", &effect->mix, 0.0f, 1.0f, "Delay/Mix");
        setSize(20 + 3 * (LiBeKnob::kW + 10) - 10, 220);
    }

    void timerCallback() override {
        updateSpectrum(effect->audioFifo, effect->audioFifoData, 44100.0);
        repaint();
    }

    void drawVisualiser(juce::Graphics& g, juce::Rectangle<int> b) override {
        drawSpectrum(g, b, accent);
        float time = effect->delayTimeMs.load() / 1000.0f;
        float fdbk = effect->feedback.load();
        g.setColour(juce::Colours::white);
        for(int i=0; i<8; ++i) {
            float x = b.getX() + (i * time * b.getWidth());
            if (x > b.getRight()) break;
            float h = b.getHeight() * std::pow(fdbk, i);
            g.fillRect(x, (float)b.getBottom() - h, 2.0f, h);
        }
    }
private:
    DelayEffect* effect;
    LiBeKnob kTime, kFdbk, kMix;
    void setupKnob(LiBeKnob& k, const juce::String& lbl, std::atomic<float>* ptr, float min, float max, const juce::String& id) {
        k.setup(lbl, ptr->load(), min, max, ptr->load(), id, accent);
        k.slider.onValueChange = [&k, ptr]() {
            ptr->store((float)k.slider.getValue(), std::memory_order_relaxed);
        };
        k.startTrackingParam(ptr, min, max);
        addKnob(&k);
    }
};

// ─── Chorus Editor ───────────────────────────────────────────────────────────
class ChorusEditor : public BespokeEffectEditor {
public:
    ChorusEditor(ChorusEffect* fx) : BespokeEffectEditor("Chorus", juce::Colour(0xffFFAA00)), effect(fx) {
        smoothSpectrum.fill(-100.0f);
        setupKnob(kRate, "Rate", &effect->rate, 0.0f, 99.0f, "Chorus/Rate");
        setupKnob(kDepth, "Depth", &effect->depth, 0.0f, 1.0f, "Chorus/Depth");
        setupKnob(kDelay, "Delay", &effect->centreDelay, 1.0f, 100.0f, "Chorus/Delay");
        setupKnob(kMix, "Mix", &effect->mix, 0.0f, 1.0f, "Chorus/Mix");
        setSize(20 + 4 * (LiBeKnob::kW + 10) - 10, 220);
    }
    
    void timerCallback() override {
        updateSpectrum(effect->audioFifo, effect->audioFifoData, 44100.0);
        phase += effect->rate.load() * 0.05f;
        repaint();
    }
    
    void drawVisualiser(juce::Graphics& g, juce::Rectangle<int> b) override {
        drawSpectrum(g, b, accent);
        juce::Path p;
        float depth = effect->depth.load();
        for(int x=0; x<b.getWidth(); ++x) {
            float normX = (float)x / b.getWidth();
            float y = b.getCentreY() + std::sin(normX * 10.0f + phase) * (depth * b.getHeight() * 0.4f);
            if (x==0) p.startNewSubPath(b.getX() + x, y);
            else p.lineTo(b.getX() + x, y);
        }
        g.setColour(juce::Colours::white);
        g.strokePath(p, juce::PathStrokeType(2.0f));
    }
private:
    ChorusEffect* effect;
    float phase = 0.0f;
    LiBeKnob kRate, kDepth, kDelay, kMix;
    void setupKnob(LiBeKnob& k, const juce::String& lbl, std::atomic<float>* ptr, float min, float max, const juce::String& id) {
        k.setup(lbl, ptr->load(), min, max, ptr->load(), id, accent);
        k.slider.onValueChange = [&k, ptr]() {
            ptr->store((float)k.slider.getValue(), std::memory_order_relaxed);
        };
        k.startTrackingParam(ptr, min, max);
        addKnob(&k);
    }
};

// ─── Filter Editor ───────────────────────────────────────────────────────────
class FilterEditor : public BespokeEffectEditor {
public:
    FilterEditor(FilterEffect* fx) : BespokeEffectEditor("Filter", juce::Colour(0xff00E676)), effect(fx) {
        smoothSpectrum.fill(-100.0f);
        setupKnob(kCutoff, "Cutoff", &effect->cutoff, 20.0f, 20000.0f, "Filter/Cutoff");
        setupKnob(kRes, "Res", &effect->resonance, 0.1f, 10.0f, "Filter/Resonance");
        setSize(20 + 2 * (LiBeKnob::kW + 10) - 10, 220);
    }

    void timerCallback() override {
        updateSpectrum(effect->audioFifo, effect->audioFifoData, effect->currentSampleRate);
        repaint();
    }

    void drawVisualiser(juce::Graphics& g, juce::Rectangle<int> b) override {
        drawSpectrum(g, b, accent);
        
        float sr = effect->currentSampleRate > 0 ? effect->currentSampleRate : 44100.0;
        auto coef = juce::dsp::IIR::Coefficients<float>::makeLowPass(sr, effect->cutoff.load(), effect->resonance.load());
        
        juce::Path p;
        for (int x = 0; x < b.getWidth(); ++x) {
            float freq = juce::mapToLog10(float(x) / b.getWidth(), 20.0f, 20000.0f);
            double mag = coef->getMagnitudeForFrequency(freq, sr);
            float db = juce::Decibels::gainToDecibels(mag);
            float y = juce::jmap(db, -24.0f, 24.0f, (float)b.getBottom(), (float)b.getY());
            y = juce::jlimit((float)b.getY(), (float)b.getBottom(), y);
            if (x == 0) p.startNewSubPath(b.getX() + x, y);
            else p.lineTo(b.getX() + x, y);
        }
        g.setColour(juce::Colours::white);
        g.strokePath(p, juce::PathStrokeType(2.0f));
    }
private:
    FilterEffect* effect;
    LiBeKnob kCutoff, kRes;
    void setupKnob(LiBeKnob& k, const juce::String& lbl, std::atomic<float>* ptr, float min, float max, const juce::String& id) {
        k.setup(lbl, ptr->load(), min, max, ptr->load(), id, accent);
        k.slider.onValueChange = [&k, ptr]() {
            ptr->store((float)k.slider.getValue(), std::memory_order_relaxed);
        };
        k.startTrackingParam(ptr, min, max);
        addKnob(&k);
    }
};

// ─── Compressor Editor ───────────────────────────────────────────────────────
class CompressorEditor : public BespokeEffectEditor {
public:
    // Fixed visual metrics shared by constructor and resized()
    static constexpr int kVisH   = 100;  // fixed visualiser height (px)
    static constexpr int kComboH = 22;
    static constexpr int kBotPad = 10;
    // Knob row y = header(38) + kVisH(100) + gap(15) = 153
    // Knob row bottom = 153 + kH(56) = 209
    // Combo y = 209 + 8 = 217
    // Total height = 217 + kComboH(22) + kBotPad(10) = 249
    static constexpr int kKnobY  = 38 + kVisH + 15;
    static constexpr int kComboY = kKnobY + LiBeKnob::kH + 8;
    static constexpr int kTotalH = kComboY + kComboH + kBotPad;
    static constexpr int kNumKnobs = 6;
    static constexpr int kTotalW = 20 + kNumKnobs * (LiBeKnob::kW + 10) - 10;

    CompressorEditor(CompressorEffect* fx) : BespokeEffectEditor("Compressor", juce::Colour(0xffFF5252)), effect(fx) {
        setupKnob(kThresh,  "Thresh",  &effect->threshold, -60.0f,   0.0f, "Compressor/Threshold");
        setupKnob(kRatio,   "Ratio",   &effect->ratio,      1.0f,   20.0f, "Compressor/Ratio");
        setupKnob(kAttack,  "Attack",  &effect->attack,     0.1f,  100.0f, "Compressor/Attack");
        setupKnob(kRelease, "Release", &effect->release,   10.0f, 1000.0f, "Compressor/Release");
        setupKnob(kMakeup,  "Makeup",  &effect->makeup,     0.0f,   24.0f, "Compressor/Makeup");
        setupKnob(kMix,     "Mix %",   &effect->mix,        0.0f,  100.0f, "Compressor/Mix");

        // ── Sidechain source ComboBox ────────────────────────────────────────
        scCombo.addItem("Self (no sidechain)", 1);
        scCombo.setSelectedId(1, juce::dontSendNotification);
        scCombo.setColour(juce::ComboBox::backgroundColourId, juce::Colour(0xff1A1A2A));
        scCombo.setColour(juce::ComboBox::textColourId, juce::Colours::lightgrey);
        scCombo.setColour(juce::ComboBox::outlineColourId, accent.withAlpha(0.5f));
        scCombo.onChange = [this] {
            int selId    = scCombo.getSelectedId();
            int srcTrack = selId - 2; // id 1 → -1 (self), id 2 → track 0, etc.
            if (effect->onSidechainSourceChanged) effect->onSidechainSourceChanged(srcTrack);
        };
        addAndMakeVisible(scCombo);

        setSize(kTotalW, kTotalH);
        smoothGr = 0.0f;
    }

    // ── Override so the base-class knob layout uses the fixed vis height ────
    juce::Rectangle<int> getVisualiserBounds() const override {
        return { 10, 38, getWidth() - 20, kVisH };
    }

    // Called by MainComponent::showDeviceEditorForTrack when opening this editor.
    void refreshSidechainSources(const juce::StringArray& trackNames, int selfTrackIdx)
    {
        scCombo.clear(juce::dontSendNotification);
        scCombo.addItem("Self (no sidechain)", 1);
        for (int i = 0; i < trackNames.size(); ++i)
        {
            if (i == selfTrackIdx) continue;
            scCombo.addItem(trackNames[i], i + 2);
        }
        int curSrc = effect->sidechainSourceIndex.load(std::memory_order_relaxed);
        scCombo.setSelectedId((curSrc < 0) ? 1 : (curSrc + 2), juce::dontSendNotification);
    }

    void timerCallback() override {
        float gr = effect->gainReductionDb.load();
        smoothGr = smoothGr * 0.7f + gr * 0.3f;
        repaint();
    }

    void resized() override {
        BespokeEffectEditor::resized(); // lays knobs starting at getVisualiserBounds().bottom + 15
        scCombo.setBounds(10, kComboY, getWidth() - 20, kComboH);
    }

    void drawVisualiser(juce::Graphics& g, juce::Rectangle<int> b) override {
        // GR meter — centered narrow bar
        juce::Rectangle<float> meterBg(b.getCentreX() - 15.0f, b.getY() + 8.0f, 30.0f, b.getHeight() - 16.0f);
        g.setColour(juce::Colour(0xff1A1A2A));
        g.fillRect(meterBg);
        g.setColour(juce::Colour(0xff333344));
        g.drawRect(meterBg, 1.0f);

        const float maxGr  = 24.0f;
        const float grNorm = juce::jlimit(0.0f, 1.0f, smoothGr / maxGr);
        g.setColour(accent);
        g.fillRect(meterBg.withHeight(meterBg.getHeight() * grNorm));

        // dB tick marks
        g.setColour(juce::Colours::white.withAlpha(0.4f));
        for (float db = 0; db <= maxGr; db += 6.0f) {
            float y = meterBg.getY() + meterBg.getHeight() * (db / maxGr);
            g.drawLine(meterBg.getX() - 4, y, meterBg.getX(), y, 1.0f);
            g.drawLine(meterBg.getRight(), y, meterBg.getRight() + 4, y, 1.0f);
        }

        // "SC" badge when sidechain is active
        if (effect->sidechainSourceIndex.load(std::memory_order_relaxed) >= 0) {
            g.setColour(accent.brighter(0.4f));
            g.setFont(juce::Font(juce::FontOptions(9.0f, juce::Font::bold)));
            g.drawText("SC", b.getRight() - 22, b.getY() + 4, 18, 10, juce::Justification::centred);
        }
    }

private:
    CompressorEffect* effect;
    float smoothGr { 0.0f };
    LiBeKnob kThresh, kRatio, kAttack, kRelease, kMakeup, kMix;
    juce::ComboBox scCombo;
    void setupKnob(LiBeKnob& k, const juce::String& lbl, std::atomic<float>* ptr, float min, float max, const juce::String& id) {
        k.setup(lbl, ptr->load(), min, max, ptr->load(), id, accent);
        k.slider.onValueChange = [&k, ptr]() {
            ptr->store((float)k.slider.getValue(), std::memory_order_relaxed);
        };
        k.startTrackingParam(ptr, min, max);
        addKnob(&k);
    }
};

// ─── Limiter Editor ──────────────────────────────────────────────────────────
class LimiterEditor : public BespokeEffectEditor {
public:
    LimiterEditor(LimiterEffect* fx) : BespokeEffectEditor("Limiter", juce::Colour(0xffFF6D00)), effect(fx) {
        setupKnob(kThresh, "Thresh", &effect->threshold, -40.0f, 0.0f, "Limiter/Threshold");
        setupKnob(kRelease, "Release", &effect->release, 10.0f, 1000.0f, "Limiter/Release");
        setSize(20 + 2 * (LiBeKnob::kW + 10) - 10, 220);
        smoothIn = -100.0f;
        smoothOut = -100.0f;
    }

    void timerCallback() override {
        float inDb = effect->inputLevelDb.load();
        float outDb = effect->outputLevelDb.load();
        smoothIn = std::max(inDb, smoothIn - 1.0f); // Fast attack, slow decay
        smoothOut = std::max(outDb, smoothOut - 1.0f);
        repaint();
    }

    void drawVisualiser(juce::Graphics& g, juce::Rectangle<int> b) override {
        // Horizontal rulers
        juce::Rectangle<float> inBar(b.getX() + 10.0f, b.getY() + 20.0f, b.getWidth() - 20.0f, 10.0f);
        juce::Rectangle<float> outBar(b.getX() + 10.0f, b.getY() + 50.0f, b.getWidth() - 20.0f, 10.0f);
        
        g.setColour(juce::Colour(0xff1A1A2A));
        g.fillRect(inBar);
        g.fillRect(outBar);

        auto drawMeter = [&](juce::Rectangle<float> r, float db) {
            float norm = juce::jmap(db, -60.0f, 0.0f, 0.0f, 1.0f);
            norm = juce::jlimit(0.0f, 1.0f, norm);
            g.setColour(accent);
            g.fillRect(r.withWidth(r.getWidth() * norm));
        };
        drawMeter(inBar, smoothIn);
        drawMeter(outBar, smoothOut);

        g.setColour(juce::Colours::white.withAlpha(0.7f));
        g.setFont(10.0f);
        g.drawText("IN", inBar.translated(0, -12).toNearestInt(), juce::Justification::bottomLeft);
        g.drawText("OUT", outBar.translated(0, -12).toNearestInt(), juce::Justification::bottomLeft);

        // Threshold line
        float thrNorm = juce::jmap(effect->threshold.load(), -60.0f, 0.0f, 0.0f, 1.0f);
        thrNorm = juce::jlimit(0.0f, 1.0f, thrNorm);
        float thrX = inBar.getX() + inBar.getWidth() * thrNorm;
        g.setColour(juce::Colours::white);
        g.drawLine(thrX, inBar.getY() - 5, thrX, outBar.getBottom() + 5, 2.0f);
    }
private:
    LimiterEffect* effect;
    float smoothIn, smoothOut;
    LiBeKnob kThresh, kRelease;
    void setupKnob(LiBeKnob& k, const juce::String& lbl, std::atomic<float>* ptr, float min, float max, const juce::String& id) {
        k.setup(lbl, ptr->load(), min, max, ptr->load(), id, accent);
        k.slider.onValueChange = [&k, ptr]() {
            ptr->store((float)k.slider.getValue(), std::memory_order_relaxed);
        };
        k.startTrackingParam(ptr, min, max);
        addKnob(&k);
    }
};

// ─── Phaser Editor ───────────────────────────────────────────────────────────
class PhaserEditor : public BespokeEffectEditor {
public:
    PhaserEditor(PhaserEffect* fx) : BespokeEffectEditor("Phaser", juce::Colour(0xffCE93D8)), effect(fx) {
        smoothSpectrum.fill(-100.0f);
        setupKnob(kRate, "Rate", &effect->rate, 0.01f, 10.0f, "Phaser/Rate");
        setupKnob(kDepth, "Depth", &effect->depth, 0.0f, 1.0f, "Phaser/Depth");
        setupKnob(kFreq, "Freq", &effect->centreFreq, 50.0f, 5000.0f, "Phaser/Frequency");
        setupKnob(kFdbk, "Fdbk", &effect->feedback, -1.0f, 1.0f, "Phaser/Feedback");
        setupKnob(kMix, "Mix", &effect->mix, 0.0f, 1.0f, "Phaser/Mix");
        setSize(20 + 5 * (LiBeKnob::kW + 10) - 10, 220);
    }

    void timerCallback() override {
        updateSpectrum(effect->audioFifo, effect->audioFifoData, 44100.0);
        phase += effect->rate.load() * 0.05f;
        repaint();
    }

    void drawVisualiser(juce::Graphics& g, juce::Rectangle<int> b) override {
        drawSpectrum(g, b, accent);
        
        float cf = effect->centreFreq.load();
        float dp = effect->depth.load();
        
        juce::Path p;
        for (int x = 0; x < b.getWidth(); ++x) {
            float freq = juce::mapToLog10(float(x) / b.getWidth(), 20.0f, 20000.0f);
            
            // Simulated notches shifting with LFO
            float lfo = std::sin(phase) * dp;
            float shiftCf = cf * std::pow(2.0f, lfo * 3.0f);
            
            float mag = 1.0f;
            // Fake 4 stages
            for (int stage = 0; stage < 2; ++stage) {
                float notchFreq = shiftCf * std::pow(3.0f, stage);
                float dist = std::abs(std::log2(freq / notchFreq));
                mag *= juce::jlimit(0.0f, 1.0f, dist * 2.0f);
            }
            
            float db = juce::Decibels::gainToDecibels(mag + 0.01f);
            float y = juce::jmap(db, -24.0f, 0.0f, (float)b.getBottom(), b.getY() + 10.0f);
            y = juce::jlimit((float)b.getY(), (float)b.getBottom(), y);
            
            if (x == 0) p.startNewSubPath(b.getX() + x, y);
            else p.lineTo(b.getX() + x, y);
        }
        g.setColour(juce::Colours::white);
        g.strokePath(p, juce::PathStrokeType(2.0f));
    }
private:
    PhaserEffect* effect;
    float phase = 0.0f;
    LiBeKnob kRate, kDepth, kFreq, kFdbk, kMix;
    void setupKnob(LiBeKnob& k, const juce::String& lbl, std::atomic<float>* ptr, float min, float max, const juce::String& id) {
        k.setup(lbl, ptr->load(), min, max, ptr->load(), id, accent);
        k.slider.onValueChange = [&k, ptr]() {
            ptr->store((float)k.slider.getValue(), std::memory_order_relaxed);
        };
        k.startTrackingParam(ptr, min, max);
        addKnob(&k);
    }
};

// ─── Saturation Editor ───────────────────────────────────────────────────────
class SaturationEditor : public BespokeEffectEditor {
public:
    SaturationEditor(SaturationEffect* fx) : BespokeEffectEditor("Saturation", juce::Colour(0xffFFD54F)), effect(fx) {
        smoothSpectrum.fill(-100.0f);
        setupKnob(kDrive, "Drive", &effect->drive, 1.0f, 20.0f, "Saturation/Drive");
        setupKnob(kMix, "Mix", &effect->mix, 0.0f, 1.0f, "Saturation/Mix");
        setSize(20 + 2 * (LiBeKnob::kW + 10) - 10, 220);
    }

    void timerCallback() override {
        updateSpectrum(effect->audioFifo, effect->audioFifoData, 44100.0);
        repaint();
    }

    void drawVisualiser(juce::Graphics& g, juce::Rectangle<int> b) override {
        drawSpectrum(g, b, accent);
        
        float drv = effect->drive.load();
        
        juce::Path p;
        for (int x = 0; x < b.getWidth(); ++x) {
            float in = juce::jmap((float)x, 0.0f, (float)b.getWidth(), -1.0f, 1.0f);
            float out = std::tanh(in * drv);
            
            float py = juce::jmap(out, -1.0f, 1.0f, (float)b.getBottom(), (float)b.getY());
            if (x == 0) p.startNewSubPath(b.getX() + x, py);
            else p.lineTo(b.getX() + x, py);
        }
        g.setColour(juce::Colours::white);
        g.strokePath(p, juce::PathStrokeType(2.0f));
    }
private:
    SaturationEffect* effect;
    LiBeKnob kDrive, kMix;
    void setupKnob(LiBeKnob& k, const juce::String& lbl, std::atomic<float>* ptr, float min, float max, const juce::String& id) {
        k.setup(lbl, ptr->load(), min, max, ptr->load(), id, accent);
        k.slider.onValueChange = [&k, ptr]() {
            ptr->store((float)k.slider.getValue(), std::memory_order_relaxed);
        };
        k.startTrackingParam(ptr, min, max);
        addKnob(&k);
    }
};
