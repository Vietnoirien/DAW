#pragma once
#include <JuceHeader.h>
#include "WavetableSynthProcessor.h"

// ============================================================
// Wavetable LookAndFeel (cyan/teal theme)
// ============================================================
class WTLookAndFeel : public juce::LookAndFeel_V4 {
public:
    WTLookAndFeel() {
        setColour(juce::Slider::rotarySliderFillColourId,    juce::Colour(0xff00E5CC));
        setColour(juce::Slider::rotarySliderOutlineColourId, juce::Colour(0xff0A2A28));
        setColour(juce::Slider::thumbColourId,               juce::Colour(0xff88FFEE));
        setColour(juce::ComboBox::backgroundColourId,        juce::Colour(0xff0A1A18));
        setColour(juce::ComboBox::textColourId,              juce::Colour(0xff00E5CC));
        setColour(juce::ComboBox::outlineColourId,           juce::Colour(0xff0A3030));
        setColour(juce::ToggleButton::textColourId,          juce::Colour(0xff00E5CC));
        setColour(juce::ToggleButton::tickColourId,          juce::Colour(0xff00E5CC));
    }
    void drawRotarySlider(juce::Graphics& g, int x, int y, int w, int h,
                          float pos, float sa, float ea, juce::Slider&) override {
        float cx = x + w * 0.5f, cy = y + h * 0.5f;
        float r  = (float)juce::jmin(w / 2, h / 2) - 3.0f;
        float angle = sa + pos * (ea - sa);
        juce::Path arc;  arc.addCentredArc(cx, cy, r, r, 0, sa, ea, true);
        g.setColour(juce::Colour(0xff0A2A28)); g.strokePath(arc,  juce::PathStrokeType(3));
        juce::Path fill; fill.addCentredArc(cx, cy, r, r, 0, sa, angle, true);
        g.setColour(juce::Colour(0xff00E5CC)); g.strokePath(fill, juce::PathStrokeType(3));
        g.setColour(juce::Colour(0xff0A1A18));
        g.fillEllipse(cx - r * 0.6f, cy - r * 0.6f, r * 1.2f, r * 1.2f);
        juce::Path p; p.addRectangle(-1.5f, -r * 0.5f, 3.0f, r * 0.5f);
        p.applyTransform(juce::AffineTransform::rotation(angle).translated(cx, cy));
        g.setColour(juce::Colour(0xff88FFEE)); g.fillPath(p);
    }
};

// ============================================================
// Waveform display — dual oscilloscope showing both WT positions
// ============================================================
class WavetableDisplay : public juce::Component, public juce::Timer {
public:
    WavetableDisplay(std::atomic<float>& wtPosA, std::atomic<float>& wtPosB)
        : posA(wtPosA), posB(wtPosB) { startTimerHz(30); }
    void timerCallback() override { repaint(); }

    void paint(juce::Graphics& g) override {
        g.fillAll(juce::Colour(0xff040E0D));
        g.setColour(juce::Colour(0xff0A2A28));
        g.drawRect(getLocalBounds(), 1);

        const float h = (float)getHeight();
        const float w = (float)getWidth();

        // Divider line between upper (A) and lower (B) half
        g.setColour(juce::Colour(0xff0A2A28));
        g.drawHorizontalLine((int)(h * 0.5f), 2.0f, w - 2.0f);

        auto drawWave = [&](float wtPos, juce::Colour col, float regionY, float regionH) {
            juce::Path path;
            bool started = false;
            for (int px = 0; px < (int)w; ++px) {
                float t   = px / w;
                float smp = kGlobalWT.getMorphSample(wtPos, t);
                float yy  = regionY + regionH * 0.5f - smp * regionH * 0.42f;
                if (!started) { path.startNewSubPath((float)px, yy); started = true; }
                else            path.lineTo((float)px, yy);
            }
            g.setColour(col);
            g.strokePath(path, juce::PathStrokeType(1.5f));
        };

        drawWave(posA.load(), juce::Colour(0xff00E5CC).withAlpha(0.85f), 0.0f,     h * 0.5f);
        drawWave(posB.load(), juce::Colour(0xffFF8800).withAlpha(0.85f), h * 0.5f, h * 0.5f);

        static const char* kNames[] = {"Sine","Tri","Saw","RevSaw","Square","Pulse25","SuperSaw","Additive"};
        g.setFont(juce::FontOptions(9.0f));
        g.setColour(juce::Colour(0xff00E5CC));
        g.drawText(juce::String("A: ") + kNames[juce::jlimit(0,7,(int)posA.load())],
                   4, 2, 120, 12, juce::Justification::centredLeft);
        g.setColour(juce::Colour(0xffFF8800));
        g.drawText(juce::String("B: ") + kNames[juce::jlimit(0,7,(int)posB.load())],
                   4, (int)(h * 0.5f) + 2, 120, 12, juce::Justification::centredLeft);
    }
private:
    std::atomic<float>& posA;
    std::atomic<float>& posB;
};

// ============================================================
// Knob helper — draws a knob + label below it as a unit
// ============================================================
class LabelledKnob : public juce::Component {
public:
    juce::Slider slider;

    LabelledKnob(const juce::String& lbl, double v, double mn, double mx, double def = -1.0) : label(lbl) {
        addAndMakeVisible(slider);
        slider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        slider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        slider.setRange(mn, mx);
        slider.setValue(v, juce::dontSendNotification);
        slider.setDoubleClickReturnValue(true, def < 0.0 ? v : def);
    }

    void paint(juce::Graphics& g) override {
        g.setColour(juce::Colours::grey);
        g.setFont(juce::FontOptions(9.0f));
        g.drawText(label, 0, getHeight() - 13, getWidth(), 13, juce::Justification::centred);
    }

    void resized() override {
        slider.setBounds(0, 0, getWidth(), getHeight() - 14);
    }

    static constexpr int kW = 42; // preferred width per knob
    static constexpr int kH = 52; // preferred height (knob + label)

private:
    juce::String label;
};

// ============================================================
// WTOscPanel — single oscillator strip (horizontal layout)
// ============================================================
class WTOscPanel : public juce::Component {
public:
    WTOscPanel(const juce::String& name, WTParams::Osc& o, juce::Colour accent)
        : oscName(name), osc(o), col(accent),
          knobWT   ("WT",     o.wtPos.load(),        0,   kWTCount - 1, 0),
          knobOct  ("Oct",    o.octave.load(),        -3,  3,            0),
          knobCrs  ("Crs",    o.coarse.load(),        -24, 24,           0),
          knobLvl  ("Level",  o.level.load(),         0,   1,            0.8),
          knobPan  ("Pan",    o.pan.load(),            0,   1,            0.5),
          knobUni  ("Unison", o.unisonVoices.load(),  1,   8,            1),
          knobDet  ("Detune", o.unisonDetune.load(),  0,   1,            0.1)
    {
        addAndMakeVisible(enabledBtn);
        enabledBtn.setButtonText(name);
        enabledBtn.setToggleState(o.enabled.load(), juce::dontSendNotification);
        enabledBtn.onClick = [this] { osc.enabled.store(enabledBtn.getToggleState()); };

        for (auto* k : knobs()) addAndMakeVisible(*k);

        knobWT. slider.onValueChange = [this] { osc.wtPos.store        ((float)knobWT. slider.getValue()); };
        knobOct.slider.onValueChange = [this] { osc.octave.store       ((float)knobOct.slider.getValue()); };
        knobCrs.slider.onValueChange = [this] { osc.coarse.store       ((float)knobCrs.slider.getValue()); };
        knobLvl.slider.onValueChange = [this] { osc.level.store        ((float)knobLvl.slider.getValue()); };
        knobPan.slider.onValueChange = [this] { osc.pan.store          ((float)knobPan.slider.getValue()); };
        knobUni.slider.onValueChange = [this] { osc.unisonVoices.store ((int)  knobUni.slider.getValue()); };
        knobDet.slider.onValueChange = [this] { osc.unisonDetune.store ((float)knobDet.slider.getValue()); };
    }

    void paint(juce::Graphics& g) override {
        g.fillAll(juce::Colour(0xff040E0D));
        g.setColour(col.withAlpha(0.35f));
        g.drawRect(getLocalBounds(), 1);
        // Coloured left accent bar
        g.setColour(col.withAlpha(0.6f));
        g.fillRect(0, 0, 3, getHeight());
    }

    void resized() override {
        auto b = getLocalBounds().withTrimmedLeft(4).reduced(2);
        enabledBtn.setBounds(b.removeFromTop(22));
        b.removeFromTop(2);

        // All 7 knobs in a single horizontal row
        const int kw = LabelledKnob::kW;
        const int kh = LabelledKnob::kH;
        int x = b.getX();
        int y = b.getY();
        for (auto* k : knobs()) {
            k->setBounds(x, y, kw, kh);
            x += kw + 4;
        }
    }

    // Preferred size hint
    static int preferredWidth()  { return 4 + 7 * (LabelledKnob::kW + 4) + 4; }
    static int preferredHeight() { return 2 + 22 + 2 + LabelledKnob::kH + 4; }

private:
    std::vector<LabelledKnob*> knobs() {
        return { &knobWT, &knobOct, &knobCrs, &knobLvl, &knobPan, &knobUni, &knobDet };
    }

    juce::String       oscName;
    WTParams::Osc&     osc;
    juce::Colour       col;
    juce::ToggleButton enabledBtn;
    LabelledKnob       knobWT, knobOct, knobCrs, knobLvl, knobPan, knobUni, knobDet;
};

// ============================================================
// Section label helper
// ============================================================
static void drawSectionLabel(juce::Graphics& g, const juce::String& text, int x, int y, int w = 200)
{
    g.setColour(juce::Colour(0xff00E5CC).withAlpha(0.75f));
    g.setFont(juce::FontOptions(10.0f, juce::Font::bold));
    g.drawText(text, x, y, w, 14, juce::Justification::centredLeft);
}

// ============================================================
// WavetableSynthComponent — main editor
// Layout:
//   ┌─────────────────────────────────────────────────────────┐
//   │  WAVETABLE SYNTH  |  8 WAVEFORMS             [title]   │
//   ├────────────────┬────────────────────────────────────────┤
//   │  [Waveform     │  AMP ENV  [A][D][S][R]                │
//   │   Display]     │  FILTER ENV [A][D][S][R]              │
//   ├────────────────│  FILTER  [en] [LP▾] [Cut][Res][Env]   │
//   │  [OSC A strip] │  MASTER  [Lvl]                        │
//   ├────────────────┤                                        │
//   │  [OSC B strip] │                                        │
//   └────────────────┴────────────────────────────────────────┘
// ============================================================
class WavetableSynthComponent : public juce::Component {
public:
    explicit WavetableSynthComponent(WavetableSynthProcessor* proc)
        : processor(proc),
          oscA("OSC A", proc->params.oscA, juce::Colour(0xff00E5CC)),
          oscB("OSC B", proc->params.oscB, juce::Colour(0xffFF8800)),
          display(proc->params.oscA.wtPos, proc->params.oscB.wtPos),
          kAmp ("A",      proc->params.ampA.load(),         0.001, 5.0,   0.001),
          kAmpD("D",      proc->params.ampD.load(),         0.001, 5.0,   0.1),
          kAmpS("S",      proc->params.ampS.load(),         0.0,   1.0,   1.0),
          kAmpR("R",      proc->params.ampR.load(),         0.001, 5.0,   0.2),
          kFltA("A",      proc->params.filtA.load(),        0.001, 5.0,   0.01),
          kFltD("D",      proc->params.filtD.load(),        0.001, 5.0,   0.2),
          kFltS("S",      proc->params.filtS.load(),        0.0,   1.0,   0.5),
          kFltR("R",      proc->params.filtR.load(),        0.001, 5.0,   0.3),
          kCut ("Cutoff", proc->params.filterCutoff.load(), 20.0, 20000.0, 8000.0),
          kRes ("Res",    proc->params.filterRes.load(),    0.1,   10.0,  0.707),
          kEnv ("Env Amt",proc->params.filterEnvAmt.load(),-1.0,  1.0,   0.0),
          kMast("Master", proc->params.masterLevel.load(),  0.0,   1.0,   0.8)
    {
        setLookAndFeel(&laf);
        addAndMakeVisible(display);
        addAndMakeVisible(oscA);
        addAndMakeVisible(oscB);

        auto& p = proc->params;

        // Skew & connect amp env
        kAmp. slider.setSkewFactorFromMidPoint(0.5);
        kAmpD.slider.setSkewFactorFromMidPoint(0.5);
        kAmpR.slider.setSkewFactorFromMidPoint(0.5);
        kFltA.slider.setSkewFactorFromMidPoint(0.5);
        kFltD.slider.setSkewFactorFromMidPoint(0.5);
        kFltR.slider.setSkewFactorFromMidPoint(0.5);
        kCut. slider.setSkewFactorFromMidPoint(1000.0);

        kAmp. slider.onValueChange = [&p, this] { p.ampA.store        ((float)kAmp. slider.getValue()); };
        kAmpD.slider.onValueChange = [&p, this] { p.ampD.store        ((float)kAmpD.slider.getValue()); };
        kAmpS.slider.onValueChange = [&p, this] { p.ampS.store        ((float)kAmpS.slider.getValue()); };
        kAmpR.slider.onValueChange = [&p, this] { p.ampR.store        ((float)kAmpR.slider.getValue()); };
        kFltA.slider.onValueChange = [&p, this] { p.filtA.store       ((float)kFltA.slider.getValue()); };
        kFltD.slider.onValueChange = [&p, this] { p.filtD.store       ((float)kFltD.slider.getValue()); };
        kFltS.slider.onValueChange = [&p, this] { p.filtS.store       ((float)kFltS.slider.getValue()); };
        kFltR.slider.onValueChange = [&p, this] { p.filtR.store       ((float)kFltR.slider.getValue()); };
        kCut. slider.onValueChange = [&p, this] { p.filterCutoff.store((float)kCut. slider.getValue()); };
        kRes. slider.onValueChange = [&p, this] { p.filterRes.store   ((float)kRes. slider.getValue()); };
        kEnv. slider.onValueChange = [&p, this] { p.filterEnvAmt.store((float)kEnv. slider.getValue()); };
        kMast.slider.onValueChange = [&p, this] { p.masterLevel.store ((float)kMast.slider.getValue()); };

        for (auto* k : allKnobs()) addAndMakeVisible(*k);

        // Filter toggle + type combo
        addAndMakeVisible(filterEnabled);
        filterEnabled.setButtonText("Filter On");
        filterEnabled.setToggleState(p.filterEnabled.load(), juce::dontSendNotification);
        filterEnabled.onClick = [&p, this] { p.filterEnabled.store(filterEnabled.getToggleState()); };

        addAndMakeVisible(filterTypeCombo);
        filterTypeCombo.addItem("LP", 1);
        filterTypeCombo.addItem("HP", 2);
        filterTypeCombo.addItem("BP", 3);
        filterTypeCombo.setSelectedId(p.filterType.load() + 1, juce::dontSendNotification);
        filterTypeCombo.onChange = [&p, this] { p.filterType.store(filterTypeCombo.getSelectedId() - 1); };

        setSize(760, 370);
    }

    ~WavetableSynthComponent() override { setLookAndFeel(nullptr); }

    void paint(juce::Graphics& g) override {
        g.fillAll(juce::Colour(0xff030A09));

        // Title bar
        g.setColour(juce::Colour(0xff0A1A18));
        g.fillRect(0, 0, getWidth(), 30);
        g.setColour(juce::Colour(0xff00E5CC));
        g.setFont(juce::FontOptions(14.0f, juce::Font::bold));
        g.drawText("WAVETABLE SYNTH  |  8 WAVEFORMS", 10, 0, getWidth() - 10, 30,
                   juce::Justification::centredLeft);
        g.setColour(juce::Colour(0xff0A2A28));
        g.drawHorizontalLine(30, 0.0f, (float)getWidth());

        // Column divider
        g.setColour(juce::Colour(0xff0A2A28));
        g.drawVerticalLine(kLeftColW, 30.0f, (float)getHeight());

        // Section labels (painted after layout so positions are known)
        drawSectionLabel(g, "AMP ENV",    kAmp. getBoundsInParent().getX(), kAmp. getBoundsInParent().getY() - 15);
        drawSectionLabel(g, "FILTER ENV", kFltA.getBoundsInParent().getX(), kFltA.getBoundsInParent().getY() - 15);
        drawSectionLabel(g, "FILTER",     kCut. getBoundsInParent().getX(), kCut. getBoundsInParent().getY() - 15);
        drawSectionLabel(g, "MASTER",     kMast.getBoundsInParent().getX(), kMast.getBoundsInParent().getY() - 15);
    }

    void resized() override {
        const int titleH = 30;
        auto bounds = getLocalBounds().withTrimmedTop(titleH);

        // ── Left column: display + OSC A + OSC B ─────────────────────────
        auto leftCol = bounds.removeFromLeft(kLeftColW).reduced(4, 4);
        display.setBounds(leftCol.removeFromTop(70));
        leftCol.removeFromTop(4);
        const int oscH = WTOscPanel::preferredHeight();
        oscA.setBounds(leftCol.removeFromTop(oscH));
        leftCol.removeFromTop(4);
        oscB.setBounds(leftCol.removeFromTop(oscH));

        // ── Right column: env + filter sections ──────────────────────────
        auto right = bounds.reduced(10, 6);
        const int kw = LabelledKnob::kW;
        const int kh = LabelledKnob::kH;

        // Amp env
        right.removeFromTop(16); // space for label
        auto ampRow = right.removeFromTop(kh);
        layoutKnobRow(ampRow, { &kAmp, &kAmpD, &kAmpS, &kAmpR }, kw, kh);

        // Filter env
        right.removeFromTop(20); // space for label
        auto fltEnvRow = right.removeFromTop(kh);
        layoutKnobRow(fltEnvRow, { &kFltA, &kFltD, &kFltS, &kFltR }, kw, kh);

        // Filter controls
        right.removeFromTop(20); // space for label
        auto filtCtrlRow = right.removeFromTop(24);
        filterEnabled.setBounds(filtCtrlRow.removeFromLeft(80));
        filtCtrlRow.removeFromLeft(4);
        filterTypeCombo.setBounds(filtCtrlRow.removeFromLeft(55));

        right.removeFromTop(4);
        auto filtKnobRow = right.removeFromTop(kh);
        layoutKnobRow(filtKnobRow, { &kCut, &kRes, &kEnv }, kw, kh);

        // Master level
        right.removeFromTop(20); // space for label
        layoutKnobRow(right.removeFromTop(kh), { &kMast }, kw, kh);
    }

private:
    static constexpr int kLeftColW = 340;

    void layoutKnobRow(juce::Rectangle<int> row,
                       std::initializer_list<LabelledKnob*> knobs,
                       int kw, int kh)
    {
        int x = row.getX();
        int y = row.getY();
        for (auto* k : knobs) {
            k->setBounds(x, y, kw, kh);
            x += kw + 6;
        }
    }

    std::vector<LabelledKnob*> allKnobs() {
        return { &kAmp, &kAmpD, &kAmpS, &kAmpR,
                 &kFltA, &kFltD, &kFltS, &kFltR,
                 &kCut, &kRes, &kEnv, &kMast };
    }

    WavetableSynthProcessor* processor;
    WTLookAndFeel      laf;
    WavetableDisplay   display;
    WTOscPanel         oscA, oscB;

    LabelledKnob kAmp, kAmpD, kAmpS, kAmpR;
    LabelledKnob kFltA, kFltD, kFltS, kFltR;
    LabelledKnob kCut, kRes, kEnv;
    LabelledKnob kMast;

    juce::ToggleButton filterEnabled;
    juce::ComboBox     filterTypeCombo;
};

inline std::unique_ptr<juce::Component> WavetableSynthProcessor::createEditor() {
    return std::make_unique<WavetableSynthComponent>(this);
}
