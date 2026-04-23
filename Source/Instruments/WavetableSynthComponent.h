#pragma once
#include <JuceHeader.h>
#include "WavetableSynthProcessor.h"
#include "../UI/LiBeLookAndFeel.h"

// ─── Wavetable display ────────────────────────────────────────────────────────
class WavetableDisplay : public juce::Component, public juce::Timer {
public:
    WavetableDisplay(std::atomic<float>& posA, std::atomic<float>& posB)
        : pA(posA), pB(posB) { startTimerHz(30); }
    void timerCallback() override { repaint(); }

    void paint(juce::Graphics& g) override {
        g.fillAll(juce::Colour(0xff040D0C));
        g.setColour(juce::Colour(0xff0A2A28)); g.drawRect(getLocalBounds(), 1);

        float h = (float)getHeight(), w = (float)getWidth();
        g.drawHorizontalLine((int)(h * 0.5f), 2.0f, w - 2.0f);

        static const char* names[] = {"Sine","Tri","Saw","RevSaw","Square","Pulse25","SuperSaw","Additive"};
        auto drawWave = [&](float pos, juce::Colour col, float ry, float rh) {
            juce::Path p; bool started = false;
            for (int x = 0; x < (int)w; ++x) {
                float t = x / w;
                float y = ry + rh * 0.5f - kGlobalWT.getMorphSample(pos, t) * rh * 0.42f;
                if (!started) { p.startNewSubPath((float)x, y); started = true; }
                else p.lineTo((float)x, y);
            }
            g.setColour(col); g.strokePath(p, juce::PathStrokeType(1.5f));
        };
        drawWave(pA.load(), juce::Colour(0xff00E5CC).withAlpha(0.85f), 0.0f,   h * 0.5f);
        drawWave(pB.load(), juce::Colour(0xffFF8800).withAlpha(0.85f), h*0.5f, h * 0.5f);

        g.setFont(juce::FontOptions(9.0f));
        g.setColour(juce::Colour(0xff00E5CC));
        g.drawText(juce::String("A: ") + names[juce::jlimit(0,7,(int)pA.load())], 4, 2, 130, 11, juce::Justification::centredLeft);
        g.setColour(juce::Colour(0xffFF8800));
        g.drawText(juce::String("B: ") + names[juce::jlimit(0,7,(int)pB.load())], 4, (int)(h*0.5f)+2, 130, 11, juce::Justification::centredLeft);
    }
private:
    std::atomic<float>& pA; std::atomic<float>& pB;
};

// ─── WTOscPanel — one oscillator strip ───────────────────────────────────────
class WTOscPanel : public juce::Component {
public:
    WTOscPanel(const juce::String& name, WTParams::Osc& o, juce::Colour col, LiBeLookAndFeel& laf)
        : oscName(name), osc(o), accent(col)
    {
        setLookAndFeel(&laf);
        addAndMakeVisible(enabledBtn);
        enabledBtn.setButtonText(name);
        enabledBtn.setToggleState(o.enabled.load(), juce::dontSendNotification);
        enabledBtn.onClick = [this] { osc.enabled.store(enabledBtn.getToggleState()); };

        auto sk = [&](LiBeKnob& k, const char* lbl, double v, double mn, double mx, double dv, const char* pid) {
            k.setup(lbl, v, mn, mx, dv, "WT/" + name + "/" + pid, col);
            addAndMakeVisible(k);
        };
        sk(kWT,  "WT",     o.wtPos.load(),       0,   kWTCount-1, 0,   "Position");
        sk(kOct, "Oct",    o.octave.load(),       -3,  3,          0,   "Octave");
        sk(kCrs, "Crs",    o.coarse.load(),       -24, 24,         0,   "Coarse");
        sk(kLvl, "Level",  o.level.load(),        0,   1,          0.8, "Level");
        sk(kPan, "Pan",    o.pan.load(),           0,   1,          0.5, "Pan");
        sk(kDet, "Detune", o.unisonDetune.load(), 0,   1,          0.1, "Detune");

        kWT.slider.onValueChange  = [this] { osc.wtPos.store       ((float)kWT.slider.getValue()); };
        kOct.slider.onValueChange = [this] { osc.octave.store      ((float)kOct.slider.getValue()); };
        kCrs.slider.onValueChange = [this] { osc.coarse.store      ((float)kCrs.slider.getValue()); };
        kLvl.slider.onValueChange = [this] { osc.level.store       ((float)kLvl.slider.getValue()); };
        kPan.slider.onValueChange = [this] { osc.pan.store         ((float)kPan.slider.getValue()); };
        kDet.slider.onValueChange = [this] { osc.unisonDetune.store((float)kDet.slider.getValue()); };
    }
    ~WTOscPanel() override { setLookAndFeel(nullptr); }

    void paint(juce::Graphics& g) override {
        g.fillAll(juce::Colour(0xff08080F));
        g.setColour(accent.withAlpha(0.3f)); g.drawRect(getLocalBounds(), 1);
        g.setColour(accent.withAlpha(0.75f)); g.fillRect(0, 0, 3, getHeight());
    }

    void resized() override {
        auto b = getLocalBounds().withTrimmedLeft(4).reduced(2);
        enabledBtn.setBounds(b.removeFromTop(20));
        b.removeFromTop(2);
        int x = b.getX(), y = b.getY();
        for (auto* k : std::initializer_list<LiBeKnob*>{ &kWT, &kOct, &kCrs, &kLvl, &kPan, &kDet }) {
            k->setBounds(x, y, LiBeKnob::kW, LiBeKnob::kH); x += LiBeKnob::kW + 3;
        }
    }

    static int preferredWidth()  { return 4 + 6 * (LiBeKnob::kW + 3) + 4; }
    static int preferredHeight() { return 2 + 20 + 2 + LiBeKnob::kH + 4; }

private:
    juce::String oscName; WTParams::Osc& osc; juce::Colour accent;
    juce::ToggleButton enabledBtn;
    LiBeKnob kWT, kOct, kCrs, kLvl, kPan, kDet;
};

// ─── WavetableSynthComponent ──────────────────────────────────────────────────
// Layout (320px tall):
//  ┌──────────────────────────────────────────────────────────┐
//  │  WAVETABLE SYNTH  |  8 WAVEFORMS              [title]   │
//  ├───────────────────┬──────────────────────────────────────┤
//  │  [Wave display]   │  AMP ENV  [A][D][S][R]              │
//  │  [OSC A strip ]   │  FILTER ENV [A][D][S][R]            │
//  │  [OSC B strip ]   │  FILTER [en][LP] [Cut][Res][Env]    │
//  │                   │  MASTER [Lvl]                        │
//  └───────────────────┴──────────────────────────────────────┘
class WavetableSynthComponent : public juce::Component {
public:
    explicit WavetableSynthComponent(WavetableSynthProcessor* proc)
        : processor(proc), laf(juce::Colour(0xff00E5CC)),
          oscA("OSC A", proc->params.oscA, juce::Colour(0xff00E5CC), laf),
          oscB("OSC B", proc->params.oscB, juce::Colour(0xffFF8800), laf),
          display(proc->params.oscA.wtPos, proc->params.oscB.wtPos)
    {
        setLookAndFeel(&laf);
        addAndMakeVisible(display);
        addAndMakeVisible(oscA);
        addAndMakeVisible(oscB);

        auto& p = proc->params;
        auto sk = [&](LiBeKnob& k, const char* lbl, double v, double mn, double mx, double dv, const char* pid) {
            k.setup(lbl, v, mn, mx, dv, pid, juce::Colour(0xff00E5CC));
            addAndMakeVisible(k);
        };
        sk(kAmpA, "A",      p.ampA.load(),         0.001, 5.0,     0.001, "WT/Global Amp/Attack");
        sk(kAmpD, "D",      p.ampD.load(),         0.001, 5.0,     0.1,   "WT/Global Amp/Decay");
        sk(kAmpS, "S",      p.ampS.load(),         0.0,   1.0,     1.0,   "WT/Global Amp/Sustain");
        sk(kAmpR, "R",      p.ampR.load(),         0.001, 5.0,     0.2,   "WT/Global Amp/Release");
        sk(kFltA, "A",      p.filtA.load(),        0.001, 5.0,     0.01,  "WT/Filter Env/Attack");
        sk(kFltD, "D",      p.filtD.load(),        0.001, 5.0,     0.2,   "WT/Filter Env/Decay");
        sk(kFltS, "S",      p.filtS.load(),        0.0,   1.0,     0.5,   "WT/Filter Env/Sustain");
        sk(kFltR, "R",      p.filtR.load(),        0.001, 5.0,     0.3,   "WT/Filter Env/Release");
        sk(kCut,  "Cutoff", p.filterCutoff.load(), 20.0,  20000.0, 8000.0,"WT/Filter/Cutoff");
        sk(kRes,  "Res",    p.filterRes.load(),    0.1,   10.0,    0.707, "WT/Filter/Resonance");
        sk(kEnv,  "Env",    p.filterEnvAmt.load(), -1.0,  1.0,     0.0,   "WT/Filter/Env Amount");
        sk(kMast, "Master", p.masterLevel.load(),  0.0,   1.0,     0.8,   "WT/Global/Master Level");

        kAmpA.slider.setSkewFactorFromMidPoint(0.5); kAmpD.slider.setSkewFactorFromMidPoint(0.5);
        kAmpR.slider.setSkewFactorFromMidPoint(0.5); kFltA.slider.setSkewFactorFromMidPoint(0.5);
        kFltD.slider.setSkewFactorFromMidPoint(0.5); kFltR.slider.setSkewFactorFromMidPoint(0.5);
        kCut.slider.setSkewFactorFromMidPoint(1000.0);

        kAmpA.slider.onValueChange = [&p,this]{ p.ampA.store       ((float)kAmpA.slider.getValue()); };
        kAmpD.slider.onValueChange = [&p,this]{ p.ampD.store       ((float)kAmpD.slider.getValue()); };
        kAmpS.slider.onValueChange = [&p,this]{ p.ampS.store       ((float)kAmpS.slider.getValue()); };
        kAmpR.slider.onValueChange = [&p,this]{ p.ampR.store       ((float)kAmpR.slider.getValue()); };
        kFltA.slider.onValueChange = [&p,this]{ p.filtA.store      ((float)kFltA.slider.getValue()); };
        kFltD.slider.onValueChange = [&p,this]{ p.filtD.store      ((float)kFltD.slider.getValue()); };
        kFltS.slider.onValueChange = [&p,this]{ p.filtS.store      ((float)kFltS.slider.getValue()); };
        kFltR.slider.onValueChange = [&p,this]{ p.filtR.store      ((float)kFltR.slider.getValue()); };
        kCut.slider.onValueChange  = [&p,this]{ p.filterCutoff.store ((float)kCut.slider.getValue()); };
        kRes.slider.onValueChange  = [&p,this]{ p.filterRes.store    ((float)kRes.slider.getValue()); };
        kEnv.slider.onValueChange  = [&p,this]{ p.filterEnvAmt.store ((float)kEnv.slider.getValue()); };
        kMast.slider.onValueChange = [&p,this]{ p.masterLevel.store  ((float)kMast.slider.getValue()); };

        addAndMakeVisible(filterEnabled);
        filterEnabled.setButtonText("Filter");
        filterEnabled.setToggleState(p.filterEnabled.load(), juce::dontSendNotification);
        filterEnabled.onClick = [&p,this]{ p.filterEnabled.store(filterEnabled.getToggleState()); };

        addAndMakeVisible(filterTypeCombo);
        filterTypeCombo.addItem("LP", 1); filterTypeCombo.addItem("HP", 2); filterTypeCombo.addItem("BP", 3);
        filterTypeCombo.setSelectedId(p.filterType.load() + 1, juce::dontSendNotification);
        filterTypeCombo.onChange = [&p,this]{ p.filterType.store(filterTypeCombo.getSelectedId()-1); };

        setSize(760, 320);
    }

    ~WavetableSynthComponent() override { setLookAndFeel(nullptr); }

    void paint(juce::Graphics& g) override {
        g.fillAll(juce::Colour(0xff07070E));
        g.setColour(juce::Colour(0xff0A1A18)); g.fillRect(0, 0, getWidth(), kTH);
        g.setColour(juce::Colour(0xff00E5CC)); g.setFont(juce::FontOptions(13.0f, juce::Font::bold));
        g.drawText("WAVETABLE SYNTH  |  8 WAVEFORMS", 12, 0, getWidth()-12, kTH, juce::Justification::centredLeft);
        g.setColour(juce::Colour(0xff202030));
        g.drawHorizontalLine(kTH, 0.0f, (float)getWidth());
        g.drawVerticalLine(kLeftW, (float)kTH, (float)getHeight());

        auto sl = [&](const char* t, int x, int y) {
            g.setColour(juce::Colour(0xff00E5CC).withAlpha(0.65f));
            g.setFont(juce::FontOptions(9.0f, juce::Font::bold));
            g.drawText(t, x, y, 160, 12, juce::Justification::centredLeft);
        };
        int rx = kLeftW + 10;
        sl("AMP ENV",    rx, kAmpA.getY() - 13);
        sl("FILTER ENV", rx, kFltA.getY() - 13);
        sl("FILTER",     rx, filterEnabled.getY() - 13);
        sl("MASTER",     rx, kMast.getY() - 13);
    }

    void resized() override {
        auto b = getLocalBounds().withTrimmedTop(kTH);

        // Left column: display + OSC A + OSC B
        auto lb = b.removeFromLeft(kLeftW).reduced(4, 3);
        display.setBounds(lb.removeFromTop(62));
        lb.removeFromTop(3);
        oscA.setBounds(lb.removeFromTop(WTOscPanel::preferredHeight()));
        lb.removeFromTop(3);
        oscB.setBounds(lb.removeFromTop(WTOscPanel::preferredHeight()));

        // Right column: env / filter sections  (gaps reduced to 8px so all fits in 290px)
        auto right = b.reduced(8, 3);
        right.removeFromTop(8);
        layoutRow(right.removeFromTop(LiBeKnob::kH), { &kAmpA, &kAmpD, &kAmpS, &kAmpR });
        right.removeFromTop(8);
        layoutRow(right.removeFromTop(LiBeKnob::kH), { &kFltA, &kFltD, &kFltS, &kFltR });
        right.removeFromTop(8);
        auto fr = right.removeFromTop(20);
        filterEnabled.setBounds(fr.removeFromLeft(52)); fr.removeFromLeft(3);
        filterTypeCombo.setBounds(fr.removeFromLeft(48));
        right.removeFromTop(3);
        layoutRow(right.removeFromTop(LiBeKnob::kH), { &kCut, &kRes, &kEnv });
        right.removeFromTop(8);
        layoutRow(right.removeFromTop(LiBeKnob::kH), { &kMast });
    }

private:
    static constexpr int kTH = 30, kLeftW = 310;

    void layoutRow(juce::Rectangle<int> row, std::initializer_list<LiBeKnob*> ks) {
        int x = row.getX(), y = row.getY();
        for (auto* k : ks) { k->setBounds(x, y, LiBeKnob::kW, LiBeKnob::kH); x += LiBeKnob::kW + 4; }
    }

    WavetableSynthProcessor* processor;
    LiBeLookAndFeel laf;
    WavetableDisplay display;
    WTOscPanel oscA, oscB;
    LiBeKnob kAmpA, kAmpD, kAmpS, kAmpR;
    LiBeKnob kFltA, kFltD, kFltS, kFltR;
    LiBeKnob kCut, kRes, kEnv, kMast;
    juce::ToggleButton filterEnabled;
    juce::ComboBox filterTypeCombo;
};

inline std::unique_ptr<juce::Component> WavetableSynthProcessor::createEditor() {
    return std::make_unique<WavetableSynthComponent>(this);
}
