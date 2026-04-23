#pragma once
#include <JuceHeader.h>
#include "OscProcessor.h"
#include "../UI/LiBeLookAndFeel.h"

// ─── OscOscPanel — single oscillator column ──────────────────────────────────
class OscOscPanel : public juce::Component {
public:
    OscOscPanel(const juce::String& name, OscParams::Osc& p, juce::Colour col, LiBeLookAndFeel& laf)
        : oscParams(p), accent(col)
    {
        setLookAndFeel(&laf);
        addAndMakeVisible(enabledBtn);
        enabledBtn.setButtonText(name);
        enabledBtn.setToggleState(p.enabled.load(), juce::dontSendNotification);
        enabledBtn.onClick = [this] { oscParams.enabled.store(enabledBtn.getToggleState()); };

        addAndMakeVisible(waveCombo);
        waveCombo.addItem("Sine",1); waveCombo.addItem("Tri",2);
        waveCombo.addItem("Saw",3);  waveCombo.addItem("Square",4); waveCombo.addItem("WT",5);
        waveCombo.setSelectedId(p.waveform.load()+1, juce::dontSendNotification);
        waveCombo.onChange = [this] { oscParams.waveform.store(waveCombo.getSelectedId()-1); };

        juce::String pre = "Oscillator/" + name + "/";
        auto sk = [&](LiBeKnob& k, const char* lbl, double v, double mn, double mx, double dv, const char* pid) {
            k.setup(lbl, v, mn, mx, dv, pre + pid, col); addAndMakeVisible(k);
        };
        sk(kOct, "Oct",    p.octave.load(),       -2, 2,   0,   "Octave");
        sk(kCrs, "Crs",    p.coarse.load(),        -24, 24, 0,   "Coarse");
        sk(kDet, "Detune", p.unisonDetune.load(),  0,  1,  0.1, "Detune");
        sk(kLvl, "Level",  p.level.load(),         0,  1,  0.8, "Level");

        kOct.slider.onValueChange = [this] { oscParams.octave.store      ((float)kOct.slider.getValue()); };
        kCrs.slider.onValueChange = [this] { oscParams.coarse.store      ((float)kCrs.slider.getValue()); };
        kDet.slider.onValueChange = [this] { oscParams.unisonDetune.store((float)kDet.slider.getValue()); };
        kLvl.slider.onValueChange = [this] { oscParams.level.store       ((float)kLvl.slider.getValue()); };
    }
    ~OscOscPanel() override { setLookAndFeel(nullptr); }

    void paint(juce::Graphics& g) override {
        g.fillAll(juce::Colour(0xff08080F));
        g.setColour(accent.withAlpha(0.3f)); g.drawRect(getLocalBounds(), 1);
        g.setColour(accent.withAlpha(0.75f)); g.fillRect(0, 0, 3, getHeight());
    }

    void resized() override {
        auto b = getLocalBounds().withTrimmedLeft(4).reduced(2);
        enabledBtn.setBounds(b.removeFromTop(18));
        b.removeFromTop(1);
        waveCombo.setBounds(b.removeFromTop(17));
        b.removeFromTop(2);
        int x = b.getX(), y = b.getY();
        for (auto* k : std::initializer_list<LiBeKnob*>{ &kOct, &kCrs, &kDet, &kLvl }) {
            k->setBounds(x, y, kOscKW, kOscKH); x += kOscKW + 3;
        }
    }

    static int preferredWidth()  { return 4 + 4*(kOscKW+3) + 4; }
    static int preferredHeight() { return 2 + 18 + 1 + 17 + 2 + kOscKH + 3; }

    static constexpr int kOscKW = 40, kOscKH = 46;

private:
    OscParams::Osc& oscParams; juce::Colour accent;
    juce::ToggleButton enabledBtn; juce::ComboBox waveCombo;
    LiBeKnob kOct, kCrs, kDet, kLvl;
};

// ─── OscEnvPanel — ADSR strip ────────────────────────────────────────────────
class OscEnvPanel : public juce::Component {
public:
    OscEnvPanel(const juce::String& name, OscParams::ADSRNode& p, juce::Colour col, LiBeLookAndFeel& laf)
        : envParams(p), title(name), accent(col)
    {
        setLookAndFeel(&laf);
        juce::String pre = "Oscillator/" + name + "/";
        auto sk = [&](LiBeKnob& k, const char* lbl, double v, double mn, double mx, const char* pid) {
            k.setup(lbl, v, mn, mx, v, pre + pid, col); addAndMakeVisible(k);
        };
        sk(kA, "A", p.attack.load(),  0.001, 5.0, "Attack");
        sk(kD, "D", p.decay.load(),   0.001, 5.0, "Decay");
        sk(kS, "S", p.sustain.load(), 0.0,   1.0, "Sustain");
        sk(kR, "R", p.release.load(), 0.001, 5.0, "Release");
        kA.slider.setSkewFactorFromMidPoint(0.5); kD.slider.setSkewFactorFromMidPoint(0.5);
        kR.slider.setSkewFactorFromMidPoint(0.5);
        kA.slider.onValueChange = [this] { envParams.attack.store ((float)kA.slider.getValue()); };
        kD.slider.onValueChange = [this] { envParams.decay.store  ((float)kD.slider.getValue()); };
        kS.slider.onValueChange = [this] { envParams.sustain.store((float)kS.slider.getValue()); };
        kR.slider.onValueChange = [this] { envParams.release.store((float)kR.slider.getValue()); };
    }
    ~OscEnvPanel() override { setLookAndFeel(nullptr); }

    void paint(juce::Graphics& g) override {
        g.fillAll(juce::Colour(0xff08080F));
        g.setColour(accent.withAlpha(0.3f)); g.drawRect(getLocalBounds(), 1);
        g.setColour(accent.withAlpha(0.75f)); g.fillRect(0, 0, 3, getHeight());
        g.setColour(accent.withAlpha(0.7f));
        g.setFont(juce::FontOptions(9.0f, juce::Font::bold));
        g.drawText(title.toUpperCase(), 6, 3, getWidth()-8, 12, juce::Justification::centredLeft);
    }

    void resized() override {
        auto b = getLocalBounds().withTrimmedLeft(4).reduced(2).withTrimmedTop(16);
        int x = b.getX(), y = b.getY();
        for (auto* k : std::initializer_list<LiBeKnob*>{ &kA, &kD, &kS, &kR }) {
            k->setBounds(x, y, LiBeKnob::kW, LiBeKnob::kH); x += LiBeKnob::kW + 3;
        }
    }

    static int preferredWidth()  { return 4 + 4*(LiBeKnob::kW+3) + 4; }
    static int preferredHeight() { return 16 + LiBeKnob::kH + 4; }

private:
    OscParams::ADSRNode& envParams; juce::String title; juce::Colour accent;
    LiBeKnob kA, kD, kS, kR;
};

// ─── OscComponent ─────────────────────────────────────────────────────────────
// Layout (320px tall):
//  ┌────────────────────────────────────────────────────────────────────────────┐
//  │  OSCILLATOR SYNTH  |  3 OSCILLATORS                            [title]    │
//  ├──────────────────────┬─────────────────────────────────────────────────────┤
//  │  OSC A  OSC B  OSC C │  AMP ENV  [A][D][S][R]                            │
//  │  [Oct][Crs][Det][Lvl]│  FILTER ENV [A][D][S][R]                          │
//  │  [waveform combo   ] │  FILTER [en][LP] [Cut][Res][Env]                  │
//  └──────────────────────┴─────────────────────────────────────────────────────┘
class OscComponent : public juce::Component {
public:
    explicit OscComponent(OscProcessor* proc)
        : processor(proc), laf(juce::Colour(0xff00D0FF)),
          oscA("OSC A", proc->params.oscA, juce::Colour(0xff00D0FF), laf),
          oscB("OSC B", proc->params.oscB, juce::Colour(0xffFF8800), laf),
          oscC("OSC C", proc->params.oscC, juce::Colour(0xffCC44FF), laf),
          ampEnv("Amp Env",    proc->params.ampEnv,    juce::Colour(0xff00D0FF), laf),
          filtEnv("Filter Env", proc->params.filterEnv, juce::Colour(0xff00D0FF), laf)
    {
        setLookAndFeel(&laf);
        addAndMakeVisible(oscA); addAndMakeVisible(oscB); addAndMakeVisible(oscC);
        addAndMakeVisible(ampEnv); addAndMakeVisible(filtEnv);

        auto& fp = proc->params.filter;
        auto sk = [&](LiBeKnob& k, const char* lbl, double v, double mn, double mx, double dv, const char* pid) {
            k.setup(lbl, v, mn, mx, dv, pid, juce::Colour(0xff00D0FF)); addAndMakeVisible(k);
        };
        sk(kCut, "Cutoff", fp.cutoff.load(),    20.0, 20000.0, 2000.0, "Oscillator/Filter/Cutoff");
        sk(kRes, "Res",    fp.resonance.load(),  0.1,  10.0,   0.707,  "Oscillator/Filter/Resonance");
        sk(kEnv, "Env",    fp.envAmount.load(), -1.0,  1.0,    0.0,    "Oscillator/Filter/Env Amount");
        kCut.slider.setSkewFactorFromMidPoint(1000.0);
        kCut.slider.onValueChange = [this] { processor->params.filter.cutoff.store   ((float)kCut.slider.getValue()); };
        kRes.slider.onValueChange = [this] { processor->params.filter.resonance.store((float)kRes.slider.getValue()); };
        kEnv.slider.onValueChange = [this] { processor->params.filter.envAmount.store((float)kEnv.slider.getValue()); };

        addAndMakeVisible(filterEnabled);
        filterEnabled.setButtonText("Filter");
        filterEnabled.setToggleState(fp.enabled.load(), juce::dontSendNotification);
        filterEnabled.onClick = [this] { processor->params.filter.enabled.store(filterEnabled.getToggleState()); };

        addAndMakeVisible(filterTypeCombo);
        filterTypeCombo.addItem("LP",1); filterTypeCombo.addItem("HP",2); filterTypeCombo.addItem("BP",3);
        filterTypeCombo.setSelectedId(fp.type.load()+1, juce::dontSendNotification);
        filterTypeCombo.onChange = [this] { processor->params.filter.type.store(filterTypeCombo.getSelectedId()-1); };

        setSize(760, 320);
    }
    ~OscComponent() override { setLookAndFeel(nullptr); }

    void paint(juce::Graphics& g) override {
        g.fillAll(juce::Colour(0xff07070E));
        g.setColour(juce::Colour(0xff0A1020)); g.fillRect(0, 0, getWidth(), kTH);
        g.setColour(juce::Colour(0xff00D0FF)); g.setFont(juce::FontOptions(13.0f, juce::Font::bold));
        g.drawText("OSCILLATOR SYNTH  |  3 OSCILLATORS", 12, 0, getWidth()-12, kTH, juce::Justification::centredLeft);
        g.setColour(juce::Colour(0xff202030));
        g.drawHorizontalLine(kTH, 0.0f, (float)getWidth());
        g.drawVerticalLine(kLeftW, (float)kTH, (float)getHeight());

        int rx = kLeftW + 10;
        auto sl = [&](const char* t, int x, int y) {
            g.setColour(juce::Colour(0xff00D0FF).withAlpha(0.65f));
            g.setFont(juce::FontOptions(9.0f, juce::Font::bold));
            g.drawText(t, x, y, 160, 12, juce::Justification::centredLeft);
        };
        sl("AMP ENV",    rx, ampEnv.getY()  - 13);
        sl("FILTER ENV", rx, filtEnv.getY() - 13);
        sl("FILTER",     rx, filterEnabled.getY() - 13);
    }

    void resized() override {
        auto b = getLocalBounds().withTrimmedTop(kTH);
        auto left = b.removeFromLeft(kLeftW).reduced(4, 3);
        const int oscH = OscOscPanel::preferredHeight();
        oscA.setBounds(left.removeFromTop(oscH)); left.removeFromTop(3);
        oscB.setBounds(left.removeFromTop(oscH)); left.removeFromTop(3);
        oscC.setBounds(left.removeFromTop(oscH));

        auto right = b.reduced(8, 3);
        right.removeFromTop(14);
        ampEnv.setBounds(right.removeFromTop(OscEnvPanel::preferredHeight()));
        right.removeFromTop(14);
        filtEnv.setBounds(right.removeFromTop(OscEnvPanel::preferredHeight()));
        right.removeFromTop(14);
        auto fr = right.removeFromTop(20);
        filterEnabled.setBounds(fr.removeFromLeft(52)); fr.removeFromLeft(3);
        filterTypeCombo.setBounds(fr.removeFromLeft(48));
        right.removeFromTop(3);
        int x = right.getX(), y = right.getY();
        for (auto* k : std::initializer_list<LiBeKnob*>{ &kCut, &kRes, &kEnv }) {
            k->setBounds(x, y, LiBeKnob::kW, LiBeKnob::kH); x += LiBeKnob::kW + 4;
        }
    }

private:
    static constexpr int kTH = 30, kLeftW = 300;
    OscProcessor* processor;
    LiBeLookAndFeel laf;
    OscOscPanel oscA, oscB, oscC;
    OscEnvPanel ampEnv, filtEnv;
    LiBeKnob kCut, kRes, kEnv;
    juce::ToggleButton filterEnabled;
    juce::ComboBox filterTypeCombo;
};

inline std::unique_ptr<juce::Component> OscProcessor::createEditor() {
    return std::make_unique<OscComponent>(this);
}
