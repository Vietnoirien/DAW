#pragma once
#include <JuceHeader.h>
#include "FMSynthProcessor.h"
#include "../UI/LiBeLookAndFeel.h"

// ============================================================
// Algorithm Diagram — operator routing visualiser
// ============================================================
class AlgorithmDiagram : public juce::Component {
public:
    void setAlgorithm(int idx) { currentAlgo = juce::jlimit(0, 31, idx); repaint(); }

    void paint(juce::Graphics& g) override {
        g.fillAll(juce::Colour(0xff08080F));
        const auto& algo = kDX7Algorithms[currentAlgo];
        const float w = (float)getWidth(), h = (float)getHeight();
        static const char* opNames[] = { "OP1", "OP2", "OP3", "OP4" };
        float boxW = w * 0.18f, boxH = h * 0.38f;
        float spacing = (w - 4.0f * boxW) / 5.0f;
        float boxY = h * 0.5f - boxH * 0.5f;
        float opX[4];
        for (int i = 0; i < 4; ++i) opX[i] = spacing + i * (boxW + spacing);

        g.setColour(juce::Colour(0xffFFAA00).withAlpha(0.55f));
        for (int t = 0; t < 4; ++t)
            for (int s = 0; s < 4; ++s)
                if ((algo.modulatedBy[t] & (1 << s)) && s != t) {
                    float sx = opX[s] + boxW * 0.5f, tx = opX[t] + boxW * 0.5f;
                    g.drawLine(sx, boxY, tx, boxY + boxH, 1.5f);
                }

        for (int i = 0; i < 4; ++i) {
            if (algo.isCarrier[i]) {
                float cx = opX[i] + boxW * 0.5f;
                g.setColour(juce::Colour(0xff55FF88).withAlpha(0.8f));
                g.drawLine(cx, boxY + boxH, cx, boxY + boxH + 12.0f, 1.5f);
            }
            bool carrier = algo.isCarrier[i];
            g.setColour(carrier ? juce::Colour(0xff1A3A18) : juce::Colour(0xff12182A));
            g.fillRoundedRectangle(opX[i], boxY, boxW, boxH, 4.0f);
            g.setColour(carrier ? juce::Colour(0xff55FF88) : juce::Colour(0xffFFAA00));
            g.drawRoundedRectangle(opX[i], boxY, boxW, boxH, 4.0f, 1.5f);
            g.setColour(juce::Colours::white);
            g.setFont(juce::FontOptions(10.0f, juce::Font::bold));
            g.drawText(opNames[i], (int)opX[i], (int)boxY, (int)boxW, (int)boxH,
                       juce::Justification::centred);
        }
    }
private:
    int currentAlgo = 0;
};

// ============================================================
// FMOperatorPanel — one operator column
// ============================================================
class FMOperatorPanel : public juce::Component {
public:
    FMOperatorPanel(int index, FMParams::Operator& op, juce::Colour accent, LiBeLookAndFeel& laf)
        : opIndex(index), opParams(op), accentCol(accent)
    {
        setLookAndFeel(&laf);
        addAndMakeVisible(enabledButton);
        enabledButton.setButtonText("OP" + juce::String(index + 1));
        enabledButton.setToggleState(op.enabled.load(), juce::dontSendNotification);
        enabledButton.onClick = [this] { opParams.enabled.store(enabledButton.getToggleState()); };

        juce::String prefix = "FM/OP " + juce::String(index + 1) + "/";
        auto setup = [&](LiBeKnob& k, const char* lbl, double v, double mn, double mx, double dv, const char* pid) {
            k.setup(lbl, v, mn, mx, dv, prefix + pid, accent);
            addAndMakeVisible(k);
        };
        setup(kRatio,   "Ratio",   op.ratio.load(),    0.125, 16.0, 1.0,   "Ratio");
        setup(kLevel,   "Level",   op.level.load(),    0.0,   1.0,  0.8,   "Level");
        setup(kFB,      "FB",      op.feedback.load(), 0.0,   1.0,  0.0,   "Feedback");
        setup(kAttack,  "A",       op.attack.load(),   0.001, 5.0,  0.001, "Attack");
        setup(kDecay,   "D",       op.decay.load(),    0.001, 5.0,  0.3,   "Decay");
        setup(kSustain, "S",       op.sustain.load(),  0.0,   1.0,  0.7,   "Sustain");
        setup(kRelease, "R",       op.release.load(),  0.001, 5.0,  0.3,   "Release");
        kAttack.slider.setSkewFactorFromMidPoint(0.5);
        kDecay.slider.setSkewFactorFromMidPoint(0.5);
        kRelease.slider.setSkewFactorFromMidPoint(0.5);

        kRatio.slider.onValueChange   = [this] { opParams.ratio.store   ((float)kRatio.slider.getValue()); };
        kLevel.slider.onValueChange   = [this] { opParams.level.store   ((float)kLevel.slider.getValue()); };
        kFB.slider.onValueChange      = [this] { opParams.feedback.store((float)kFB.slider.getValue()); };
        kAttack.slider.onValueChange  = [this] { opParams.attack.store  ((float)kAttack.slider.getValue()); };
        kDecay.slider.onValueChange   = [this] { opParams.decay.store   ((float)kDecay.slider.getValue()); };
        kSustain.slider.onValueChange = [this] { opParams.sustain.store ((float)kSustain.slider.getValue()); };
        kRelease.slider.onValueChange = [this] { opParams.release.store ((float)kRelease.slider.getValue()); };
    }

    ~FMOperatorPanel() override { setLookAndFeel(nullptr); }

    void paint(juce::Graphics& g) override {
        g.fillAll(juce::Colour(0xff08080F));
        g.setColour(accentCol.withAlpha(0.35f));
        g.drawRect(getLocalBounds(), 1);
        g.setColour(accentCol.withAlpha(0.7f));
        g.fillRect(0, 0, 3, getHeight());                  // left accent bar
        g.setColour(accentCol.withAlpha(0.5f));
        g.setFont(juce::FontOptions(9.0f, juce::Font::bold));
        g.drawText("OP " + juce::String(opIndex + 1),
                   5, 3, getWidth() - 8, 12, juce::Justification::centredLeft);
    }

    void resized() override {
        auto b = getLocalBounds().withTrimmedLeft(4).reduced(3);
        enabledButton.setBounds(b.removeFromTop(20));
        b.removeFromTop(3);

        // Row 1: Ratio / Level / FB  (3 knobs)
        layoutRow(b.removeFromTop(kOpKH), { &kRatio, &kLevel, &kFB });
        b.removeFromTop(3);

        // Row 2a: Attack / Decay
        layoutRow(b.removeFromTop(kOpKH), { &kAttack, &kDecay });
        b.removeFromTop(2);

        // Row 2b: Sustain / Release
        layoutRow(b.removeFromTop(kOpKH), { &kSustain, &kRelease });
    }

    static int preferredWidth()  { return 4 + 3*(kOpKW+4) + 4; }
    static int preferredHeight() { return 3 + 20 + 3 + kOpKH + 3 + kOpKH + 2 + kOpKH + 4; }

private:
    void layoutRow(juce::Rectangle<int> row, std::initializer_list<LiBeKnob*> knobs) {
        int x = row.getX() + 4, y = row.getY();
        for (auto* k : knobs) { k->setBounds(x, y, kOpKW, kOpKH); x += kOpKW + 4; }
    }

    // Smaller knobs fit inside the narrow op panels
    static constexpr int kOpKW = 32, kOpKH = 46;

    int opIndex; FMParams::Operator& opParams; juce::Colour accentCol;
    juce::ToggleButton enabledButton;
    LiBeKnob kRatio, kLevel, kFB, kAttack, kDecay, kSustain, kRelease;
};

// ============================================================
// FMSynthComponent — main editor
// Layout:
//  ┌────────────────────────────────────────────────────────┐
//  │  FM SYNTH  |  4-OP / 32 ALGORITHMS           [title]  │
//  ├──────────────┬─────────────────────────────────────────┤
//  │ Algo:  [▾]  │  OP1     OP2     OP3     OP4           │
//  │ [Diagram]   │  [Ratio][Ratio][Ratio][Ratio]           │
//  │ ─────────── │  [Level][Level][Level][Level]           │
//  │ MASTER      │  [FB   ][FB   ][FB   ][FB   ]           │
//  │ [Lvl]       │  ─ ADSR ────────────────────            │
//  │ AMP ENV     │  [A][D][S][R]   etc.                    │
//  │ [A][D][S][R]│                                         │
//  │ FILTER      │                                         │
//  │ [Cut][Res]  │                                         │
//  └──────────────┴─────────────────────────────────────────┘
// ============================================================
class FMSynthComponent : public juce::Component {
public:
    explicit FMSynthComponent(FMSynthProcessor* proc) : processor(proc), laf(juce::Colour(0xffFFAA00)) {
        setLookAndFeel(&laf);

        addAndMakeVisible(algoLabel);
        algoLabel.setText("Algorithm", juce::dontSendNotification);
        algoLabel.setFont(juce::FontOptions(11.0f, juce::Font::bold));

        addAndMakeVisible(algoCombo);
        for (int i = 1; i <= 32; ++i) algoCombo.addItem("Alg " + juce::String(i), i);
        algoCombo.setSelectedId(proc->params.algorithm.load() + 1, juce::dontSendNotification);
        algoCombo.onChange = [this] {
            int idx = algoCombo.getSelectedId() - 1;
            processor->params.algorithm.store(idx);
            algoDiagram.setAlgorithm(idx);
        };
        addAndMakeVisible(algoDiagram);
        algoDiagram.setAlgorithm(proc->params.algorithm.load());

        // 4 op panels with distinct accent colours
        static const juce::Colour opCols[4] = {
            juce::Colour(0xffFFAA00), juce::Colour(0xff00AAFF),
            juce::Colour(0xffFF5555), juce::Colour(0xff55FF88)
        };
        for (int i = 0; i < 4; ++i) {
            opPanels[i] = std::make_unique<FMOperatorPanel>(i, proc->params.ops[i], opCols[i], laf);
            addAndMakeVisible(*opPanels[i]);
        }

        // Left panel knobs
        auto setupK = [&](LiBeKnob& k, const char* lbl, double v, double mn, double mx, double dv, const char* pid) {
            k.setup(lbl, v, mn, mx, dv, pid, juce::Colour(0xffFFAA00));
            addAndMakeVisible(k);
        };
        setupK(kMaster, "Master",  proc->params.masterLevel.load(), 0.0, 2.0, 0.8, "FM/Global/Master Level");
        setupK(kAtkAmp, "A",   proc->params.ampAttack.load(),  0.001, 5.0, 0.001, "FM/Global Amp/Attack");
        setupK(kDecAmp, "D",   proc->params.ampDecay.load(),   0.001, 5.0, 0.3,   "FM/Global Amp/Decay");
        setupK(kSusAmp, "S",   proc->params.ampSustain.load(), 0.0,   1.0, 0.7,   "FM/Global Amp/Sustain");
        setupK(kRelAmp, "R",   proc->params.ampRelease.load(), 0.001, 5.0, 0.3,   "FM/Global Amp/Release");
        setupK(kCutoff, "Cutoff", proc->params.filterCutoff.load(),    20.0, 20000.0, 800.0, "FM/Filter/Cutoff");
        setupK(kRes,    "Res",    proc->params.filterResonance.load(), 0.1,  10.0, 1.0,     "FM/Filter/Resonance");
        kAtkAmp.slider.setSkewFactorFromMidPoint(0.5);
        kDecAmp.slider.setSkewFactorFromMidPoint(0.5);
        kRelAmp.slider.setSkewFactorFromMidPoint(0.5);
        kCutoff.slider.setSkewFactorFromMidPoint(1000.0);

        kMaster.slider.onValueChange = [this] { processor->params.masterLevel.store ((float)kMaster.slider.getValue()); };
        kAtkAmp.slider.onValueChange = [this] { processor->params.ampAttack.store   ((float)kAtkAmp.slider.getValue()); };
        kDecAmp.slider.onValueChange = [this] { processor->params.ampDecay.store    ((float)kDecAmp.slider.getValue()); };
        kSusAmp.slider.onValueChange = [this] { processor->params.ampSustain.store  ((float)kSusAmp.slider.getValue()); };
        kRelAmp.slider.onValueChange = [this] { processor->params.ampRelease.store  ((float)kRelAmp.slider.getValue()); };
        kCutoff.slider.onValueChange = [this] { processor->params.filterCutoff.store   ((float)kCutoff.slider.getValue()); };
        kRes.slider.onValueChange    = [this] { processor->params.filterResonance.store((float)kRes.slider.getValue()); };

        addAndMakeVisible(filterEnabled);
        filterEnabled.setButtonText("Filter");
        filterEnabled.setToggleState(proc->params.filterEnabled.load(), juce::dontSendNotification);
        filterEnabled.onClick = [this] { processor->params.filterEnabled.store(filterEnabled.getToggleState()); };

        addAndMakeVisible(filterTypeCombo);
        filterTypeCombo.addItem("LP", 1); filterTypeCombo.addItem("HP", 2); filterTypeCombo.addItem("BP", 3);
        filterTypeCombo.setSelectedId(proc->params.filterType.load() + 1, juce::dontSendNotification);
        filterTypeCombo.onChange = [this] { processor->params.filterType.store(filterTypeCombo.getSelectedId() - 1); };

        setSize(760, 320);
    }

    ~FMSynthComponent() override { setLookAndFeel(nullptr); }

    void paint(juce::Graphics& g) override {
        g.fillAll(juce::Colour(0xff07070E));

        g.setColour(juce::Colour(0xff0F0F1A));
        g.fillRect(0, 0, getWidth(), kTitleH);
        g.setColour(juce::Colour(0xffFFAA00));
        g.setFont(juce::FontOptions(13.0f, juce::Font::bold));
        g.drawText("FM SYNTH  |  4-OP / 32 ALGORITHMS", 12, 0, getWidth() - 12, kTitleH,
                   juce::Justification::centredLeft);
        g.setColour(juce::Colour(0xff202030));
        g.drawHorizontalLine(kTitleH, 0.0f, (float)getWidth());
        g.drawVerticalLine(kLeftW, (float)kTitleH, (float)getHeight());

        auto sectionLabel = [&](const char* txt, int x, int y) {
            g.setColour(juce::Colour(0xffFFAA00).withAlpha(0.65f));
            g.setFont(juce::FontOptions(9.0f, juce::Font::bold));
            g.drawText(txt, x, y, 160, 12, juce::Justification::centredLeft);
        };
        sectionLabel("MASTER",  kPad, kMaster.getY() - 13);
        sectionLabel("AMP ENV", kPad, kAtkAmp.getY() - 13);
        sectionLabel("FILTER",  kPad, filterEnabled.getY() - 13);
    }

    void resized() override {
        auto b = getLocalBounds().withTrimmedTop(kTitleH);
        auto left = b.removeFromLeft(kLeftW).reduced(kPad, 3);

        algoLabel.setBounds(left.removeFromTop(15));
        algoCombo.setBounds(left.removeFromTop(22));
        left.removeFromTop(4);
        algoDiagram.setBounds(left.removeFromTop(44));  // compact diagram

        left.removeFromTop(8);
        layoutLeft(left.removeFromTop(kLKH), { &kMaster });

        left.removeFromTop(8);
        layoutLeft(left.removeFromTop(kLKH), { &kAtkAmp, &kDecAmp, &kSusAmp, &kRelAmp });

        left.removeFromTop(8);
        auto filtRow = left.removeFromTop(20);
        filterEnabled.setBounds(filtRow.removeFromLeft(52));
        filtRow.removeFromLeft(3);
        filterTypeCombo.setBounds(filtRow.removeFromLeft(48));

        left.removeFromTop(2);
        layoutLeft(left.removeFromTop(kLKH), { &kCutoff, &kRes });

        b.reduce(4, 4);
        int opW = b.getWidth() / 4;
        for (int i = 0; i < 4; ++i)
            opPanels[i]->setBounds(b.removeFromLeft(opW).reduced(2));
    }

private:
    static constexpr int kTitleH = 30;
    static constexpr int kLeftW  = 210;
    static constexpr int kPad    = 8;
    // Left-panel knobs are smaller than standard LiBeKnob to fit the constrained column
    static constexpr int kLKW = 38, kLKH = 46;

    void layoutLeft(juce::Rectangle<int> row, std::initializer_list<LiBeKnob*> knobs) {
        int x = row.getX(), y = row.getY();
        for (auto* k : knobs) { k->setBounds(x, y, kLKW, kLKH); x += kLKW + 4; }
    }
    // Right-panel (full-size) knob row helper — unused for left but kept for symmetry
    void layoutRow(juce::Rectangle<int>, std::initializer_list<LiBeKnob*>) {}

    FMSynthProcessor* processor;
    LiBeLookAndFeel   laf;

    juce::Label     algoLabel;
    juce::ComboBox  algoCombo;
    AlgorithmDiagram algoDiagram;
    std::unique_ptr<FMOperatorPanel> opPanels[4];

    LiBeKnob kMaster, kAtkAmp, kDecAmp, kSusAmp, kRelAmp, kCutoff, kRes;
    juce::ToggleButton filterEnabled;
    juce::ComboBox     filterTypeCombo;
};

inline std::unique_ptr<juce::Component> FMSynthProcessor::createEditor() {
    return std::make_unique<FMSynthComponent>(this);
}
