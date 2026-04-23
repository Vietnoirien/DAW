#pragma once
#include <JuceHeader.h>
#include "FMSynthProcessor.h"

// ============================================================
// Shared look-and-feel for FM Synth (amber/gold theme)
// ============================================================
class FMLookAndFeel : public juce::LookAndFeel_V4 {
public:
    FMLookAndFeel() {
        setColour(juce::Slider::rotarySliderFillColourId,    juce::Colour(0xffFFAA00));
        setColour(juce::Slider::rotarySliderOutlineColourId, juce::Colour(0xff302810));
        setColour(juce::Slider::thumbColourId,               juce::Colour(0xffFFDD88));
        setColour(juce::ComboBox::backgroundColourId,        juce::Colour(0xff1A1408));
        setColour(juce::ComboBox::textColourId,              juce::Colour(0xffFFAA00));
        setColour(juce::ComboBox::outlineColourId,           juce::Colour(0xff3A2810));
    }

    void drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height,
                          float sliderPos, float rotaryStartAngle, float rotaryEndAngle,
                          juce::Slider&) override {
        auto radius  = (float)juce::jmin(width / 2, height / 2) - 3.0f;
        auto cx      = x + width  * 0.5f;
        auto cy      = y + height * 0.5f;
        auto angle   = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);

        // Track arc
        juce::Path arc;
        arc.addCentredArc(cx, cy, radius, radius, 0.0f, rotaryStartAngle, rotaryEndAngle, true);
        g.setColour(juce::Colour(0xff302810));
        g.strokePath(arc, juce::PathStrokeType(3.0f));

        juce::Path filled;
        filled.addCentredArc(cx, cy, radius, radius, 0.0f, rotaryStartAngle, angle, true);
        g.setColour(juce::Colour(0xffFFAA00));
        g.strokePath(filled, juce::PathStrokeType(3.0f));

        // Knob body
        g.setColour(juce::Colour(0xff1A1408));
        g.fillEllipse(cx - radius * 0.6f, cy - radius * 0.6f, radius * 1.2f, radius * 1.2f);

        // Pointer
        juce::Path p;
        p.addRectangle(-1.5f, -(radius * 0.5f), 3.0f, radius * 0.5f);
        p.applyTransform(juce::AffineTransform::rotation(angle).translated(cx, cy));
        g.setColour(juce::Colour(0xffFFDD88));
        g.fillPath(p);
    }
};

// ============================================================
// Algorithm Diagram — draws a simple visual of op routing
// ============================================================
class AlgorithmDiagram : public juce::Component {
public:
    void setAlgorithm(int algoIdx) {
        currentAlgo = juce::jlimit(0, 31, algoIdx);
        repaint();
    }

    void paint(juce::Graphics& g) override {
        g.fillAll(juce::Colour(0xff0D0A04));
        const auto& algo = kDX7Algorithms[currentAlgo];

        auto w = (float)getWidth();
        auto h = (float)getHeight();

        // Draw 4 operator boxes, right to left (op3=modulator at right)
        static const char* opNames[] = { "OP1", "OP2", "OP3", "OP4" };
        float boxW = w * 0.18f, boxH = h * 0.35f;
        float spacing = (w - 4.0f * boxW) / 5.0f;
        float boxY = h * 0.5f - boxH * 0.5f;

        float opX[4];
        for (int i = 0; i < 4; ++i)
            opX[i] = spacing + i * (boxW + spacing);

        // Draw connections first
        g.setColour(juce::Colour(0xffFFAA00).withAlpha(0.6f));
        for (int target = 0; target < 4; ++target) {
            for (int src = 0; src < 4; ++src) {
                if ((algo.modulatedBy[target] & (1 << src)) && src != target) {
                    float sx = opX[src] + boxW * 0.5f;
                    float tx = opX[target] + boxW * 0.5f;
                    float sy = boxY;
                    float ty = boxY + boxH;
                    // Arrow: draw a line + arrow head
                    g.drawLine(sx, sy, tx, ty, 1.5f);
                    // Arrow head
                    juce::Path arrow;
                    float dx = tx - sx, dy = ty - sy;
                    float len = std::sqrt(dx*dx + dy*dy);
                    if (len > 0.001f) {
                        dx /= len; dy /= len;
                        arrow.startNewSubPath(tx, ty);
                        arrow.lineTo(tx - dx * 8.0f + dy * 4.0f, ty - dy * 8.0f - dx * 4.0f);
                        arrow.lineTo(tx - dx * 8.0f - dy * 4.0f, ty - dy * 8.0f + dx * 4.0f);
                        arrow.closeSubPath();
                        g.fillPath(arrow);
                    }
                }
            }
        }

        // Carrier output arrows (downward)
        for (int i = 0; i < 4; ++i) {
            if (algo.isCarrier[i]) {
                float cx = opX[i] + boxW * 0.5f;
                float cy = boxY + boxH;
                g.setColour(juce::Colour(0xff88FF88).withAlpha(0.8f));
                g.drawLine(cx, cy, cx, cy + 14.0f, 1.5f);
                // Down arrow
                juce::Path arr;
                arr.startNewSubPath(cx, cy + 14.0f);
                arr.lineTo(cx - 4.0f, cy + 7.0f);
                arr.lineTo(cx + 4.0f, cy + 7.0f);
                arr.closeSubPath();
                g.fillPath(arr);
            }
        }

        // Draw boxes
        for (int i = 0; i < 4; ++i) {
            bool carrier  = algo.isCarrier[i];
            juce::Colour boxCol = carrier ? juce::Colour(0xff2A4A1A) : juce::Colour(0xff1A2A3A);
            juce::Colour bordCol = carrier ? juce::Colour(0xff88FF88) : juce::Colour(0xffFFAA00);
            g.setColour(boxCol);
            g.fillRoundedRectangle(opX[i], boxY, boxW, boxH, 4.0f);
            g.setColour(bordCol);
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
// Operator Panel — UI for a single FM operator
// ============================================================
class FMOperatorPanel : public juce::Component {
public:
    FMOperatorPanel(int index, FMParams::Operator& op, juce::Colour accentColour)
        : opIndex(index), opParams(op), accent(accentColour)
    {
        addAndMakeVisible(enabledButton);
        enabledButton.setButtonText("OP" + juce::String(index + 1));
        enabledButton.setToggleState(op.enabled.load(), juce::dontSendNotification);
        enabledButton.onClick = [this] { opParams.enabled.store(enabledButton.getToggleState()); };

        auto setupKnob = [&](juce::Slider& s, double v, double mn, double mx, double dv = -1.0) {
            addAndMakeVisible(s);
            s.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
            s.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
            s.setRange(mn, mx);
            s.setValue(v, juce::dontSendNotification);
            s.setDoubleClickReturnValue(true, dv < 0 ? v : dv);
        };

        setupKnob(ratioKnob,    op.ratio.load(),    0.125, 16.0, 1.0);
        setupKnob(levelKnob,    op.level.load(),    0.0,   1.0,  0.8);
        setupKnob(fbKnob,       op.feedback.load(), 0.0,   1.0,  0.0);
        setupKnob(attackKnob,   op.attack.load(),   0.001, 5.0,  0.001);
        setupKnob(decayKnob,    op.decay.load(),    0.001, 5.0,  0.3);
        setupKnob(sustainKnob,  op.sustain.load(),  0.0,   1.0,  0.7);
        setupKnob(releaseKnob,  op.release.load(),  0.001, 5.0,  0.3);

        attackKnob.setSkewFactorFromMidPoint(0.5);
        decayKnob.setSkewFactorFromMidPoint(0.5);
        releaseKnob.setSkewFactorFromMidPoint(0.5);

        ratioKnob.onValueChange   = [this] { opParams.ratio.store((float)ratioKnob.getValue()); };
        levelKnob.onValueChange   = [this] { opParams.level.store((float)levelKnob.getValue()); };
        fbKnob.onValueChange      = [this] { opParams.feedback.store((float)fbKnob.getValue()); };
        attackKnob.onValueChange  = [this] { opParams.attack.store((float)attackKnob.getValue()); };
        decayKnob.onValueChange   = [this] { opParams.decay.store((float)decayKnob.getValue()); };
        sustainKnob.onValueChange = [this] { opParams.sustain.store((float)sustainKnob.getValue()); };
        releaseKnob.onValueChange = [this] { opParams.release.store((float)releaseKnob.getValue()); };
    }

    void paint(juce::Graphics& g) override {
        g.fillAll(juce::Colour(0xff0D0A04));
        g.setColour(accent.withAlpha(0.4f));
        g.drawRect(getLocalBounds(), 1);

        g.setColour(juce::Colours::grey);
        g.setFont(juce::FontOptions(9.0f));
        struct { juce::Slider* s; const char* lbl; } labels[] = {
            { &ratioKnob, "Ratio" }, { &levelKnob, "Level" }, { &fbKnob, "FB" },
            { &attackKnob, "A" }, { &decayKnob, "D" }, { &sustainKnob, "S" }, { &releaseKnob, "R" }
        };
        for (auto& l : labels)
            g.drawText(l.lbl, l.s->getX(), l.s->getBottom(), l.s->getWidth(), 11,
                       juce::Justification::centred);
    }

    void resized() override {
        auto b = getLocalBounds().reduced(4);
        enabledButton.setBounds(b.removeFromTop(22));
        b.removeFromTop(4);

        int kw = 36, kh = 36;
        auto row1 = b.removeFromTop(kh);
        ratioKnob.setBounds(row1.removeFromLeft(kw)); row1.removeFromLeft(4);
        levelKnob.setBounds(row1.removeFromLeft(kw)); row1.removeFromLeft(4);
        fbKnob.setBounds   (row1.removeFromLeft(kw));

        b.removeFromTop(14); // space for labels
        auto row2 = b.removeFromTop(kh);
        attackKnob.setBounds (row2.removeFromLeft(kw)); row2.removeFromLeft(4);
        decayKnob.setBounds  (row2.removeFromLeft(kw)); row2.removeFromLeft(4);
        sustainKnob.setBounds(row2.removeFromLeft(kw)); row2.removeFromLeft(4);
        releaseKnob.setBounds(row2.removeFromLeft(kw));
    }

private:
    int                   opIndex;
    FMParams::Operator&   opParams;
    juce::Colour          accent;

    juce::ToggleButton enabledButton;
    juce::Slider ratioKnob, levelKnob, fbKnob;
    juce::Slider attackKnob, decayKnob, sustainKnob, releaseKnob;
};

// ============================================================
// FMSynthComponent — main editor (algorithm selector + 4 ops)
// ============================================================
class FMSynthComponent : public juce::Component {
public:
    explicit FMSynthComponent(FMSynthProcessor* proc) : processor(proc) {
        setLookAndFeel(&laf);

        // Algorithm selector
        addAndMakeVisible(algoLabel);
        algoLabel.setText("Algorithm", juce::dontSendNotification);
        algoLabel.setColour(juce::Label::textColourId, juce::Colour(0xffFFAA00));
        algoLabel.setFont(juce::FontOptions(12.0f, juce::Font::bold));

        addAndMakeVisible(algoCombo);
        for (int i = 1; i <= 32; ++i)
            algoCombo.addItem("Alg " + juce::String(i), i);
        algoCombo.setSelectedId(proc->params.algorithm.load() + 1, juce::dontSendNotification);
        algoCombo.onChange = [this] {
            int idx = algoCombo.getSelectedId() - 1;
            processor->params.algorithm.store(idx);
            algoDiagram.setAlgorithm(idx);
        };

        addAndMakeVisible(algoDiagram);
        algoDiagram.setAlgorithm(proc->params.algorithm.load());

        // 4 Operator panels
        static const juce::Colour opColours[4] = {
            juce::Colour(0xffFFAA00), juce::Colour(0xff00AAFF),
            juce::Colour(0xffFF5555), juce::Colour(0xff55FF88)
        };
        for (int i = 0; i < 4; ++i) {
            opPanels[i] = std::make_unique<FMOperatorPanel>(i, proc->params.ops[i], opColours[i]);
            addAndMakeVisible(*opPanels[i]);
        }

        // Master level
        addAndMakeVisible(masterLabel);
        masterLabel.setText("Master", juce::dontSendNotification);
        masterLabel.setColour(juce::Label::textColourId, juce::Colours::grey);
        masterLabel.setFont(juce::FontOptions(10.0f));

        addAndMakeVisible(masterKnob);
        masterKnob.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        masterKnob.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        masterKnob.setRange(0.0, 1.0);
        masterKnob.setValue(proc->params.masterLevel.load(), juce::dontSendNotification);
        masterKnob.setDoubleClickReturnValue(true, 0.8);
        masterKnob.onValueChange = [this] {
            processor->params.masterLevel.store((float)masterKnob.getValue());
        };

        // Amp ADSR
        addAndMakeVisible(ampAtkKnob);  addAndMakeVisible(ampDecKnob);
        addAndMakeVisible(ampSusKnob);  addAndMakeVisible(ampRelKnob);
        auto setupKnob = [&](juce::Slider& s, double v, double mn, double mx) {
            s.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
            s.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
            s.setRange(mn, mx);
            s.setValue(v, juce::dontSendNotification);
            s.setDoubleClickReturnValue(true, v);
        };
        setupKnob(ampAtkKnob, proc->params.ampAttack.load(),  0.001, 5.0);
        setupKnob(ampDecKnob, proc->params.ampDecay.load(),   0.001, 5.0);
        setupKnob(ampSusKnob, proc->params.ampSustain.load(), 0.0,   1.0);
        setupKnob(ampRelKnob, proc->params.ampRelease.load(), 0.001, 5.0);
        ampAtkKnob.setSkewFactorFromMidPoint(0.5);
        ampDecKnob.setSkewFactorFromMidPoint(0.5);
        ampRelKnob.setSkewFactorFromMidPoint(0.5);

        ampAtkKnob.onValueChange = [this] { processor->params.ampAttack.store ((float)ampAtkKnob.getValue()); };
        ampDecKnob.onValueChange = [this] { processor->params.ampDecay.store  ((float)ampDecKnob.getValue()); };
        ampSusKnob.onValueChange = [this] { processor->params.ampSustain.store((float)ampSusKnob.getValue()); };
        ampRelKnob.onValueChange = [this] { processor->params.ampRelease.store((float)ampRelKnob.getValue()); };

        // Filter
        addAndMakeVisible(filterEnabled);
        filterEnabled.setButtonText("Filter");
        filterEnabled.setToggleState(proc->params.filterEnabled.load(), juce::dontSendNotification);
        filterEnabled.onClick = [this] {
            processor->params.filterEnabled.store(filterEnabled.getToggleState());
        };

        addAndMakeVisible(filterTypeCombo);
        filterTypeCombo.addItem("LP", 1); filterTypeCombo.addItem("HP", 2);
        filterTypeCombo.addItem("BP", 3);
        filterTypeCombo.setSelectedId(proc->params.filterType.load() + 1, juce::dontSendNotification);
        filterTypeCombo.onChange = [this] {
            processor->params.filterType.store(filterTypeCombo.getSelectedId() - 1);
        };

        addAndMakeVisible(filterCutoffKnob); addAndMakeVisible(filterResKnob);
        setupKnob(filterCutoffKnob, proc->params.filterCutoff.load(),    20.0, 20000.0);
        setupKnob(filterResKnob,    proc->params.filterResonance.load(), 0.1,  10.0);
        filterCutoffKnob.setSkewFactorFromMidPoint(1000.0);
        filterCutoffKnob.onValueChange = [this] {
            processor->params.filterCutoff.store((float)filterCutoffKnob.getValue());
        };
        filterResKnob.onValueChange = [this] {
            processor->params.filterResonance.store((float)filterResKnob.getValue());
        };

        setSize(740, 380);
    }

    ~FMSynthComponent() override { setLookAndFeel(nullptr); }

    void paint(juce::Graphics& g) override {
        g.fillAll(juce::Colour(0xff0A0804));

        // Title bar
        g.setColour(juce::Colour(0xff1A1208));
        g.fillRect(0, 0, getWidth(), 30);
        g.setColour(juce::Colour(0xffFFAA00));
        g.setFont(juce::FontOptions(14.0f, juce::Font::bold));
        g.drawText("FM SYNTH  |  4-OP / 32 ALGORITHMS", 10, 0, getWidth() - 10, 30,
                   juce::Justification::centredLeft);

        // Section dividers
        g.setColour(juce::Colour(0xff302010));
        g.drawHorizontalLine(30, 0.0f, (float)getWidth());

        // Label rows
        g.setColour(juce::Colours::grey);
        g.setFont(juce::FontOptions(9.0f));
        int ampKnobY = ampAtkKnob.getBottom();
        g.drawText("A", ampAtkKnob.getX(), ampKnobY, ampAtkKnob.getWidth(), 11, juce::Justification::centred);
        g.drawText("D", ampDecKnob.getX(), ampKnobY, ampDecKnob.getWidth(), 11, juce::Justification::centred);
        g.drawText("S", ampSusKnob.getX(), ampKnobY, ampSusKnob.getWidth(), 11, juce::Justification::centred);
        g.drawText("R", ampRelKnob.getX(), ampKnobY, ampRelKnob.getWidth(), 11, juce::Justification::centred);
        g.drawText("Level", masterKnob.getX(), masterKnob.getBottom(), masterKnob.getWidth(), 11,
                   juce::Justification::centred);
        g.drawText("Cutoff", filterCutoffKnob.getX(), filterCutoffKnob.getBottom(),
                   filterCutoffKnob.getWidth(), 11, juce::Justification::centred);
        g.drawText("Res",    filterResKnob.getX(), filterResKnob.getBottom(),
                   filterResKnob.getWidth(), 11, juce::Justification::centred);

        // Section labels
        g.setColour(juce::Colour(0xffFFAA00).withAlpha(0.7f));
        g.setFont(juce::FontOptions(10.0f, juce::Font::bold));
        g.drawText("AMP ENV", ampAtkKnob.getX() - 2, ampAtkKnob.getY() - 16,
                   160, 14, juce::Justification::centredLeft);
        g.drawText("FILTER", filterEnabled.getX(), filterEnabled.getY() - 16,
                   80, 14, juce::Justification::centredLeft);
    }

    void resized() override {
        auto b = getLocalBounds();
        b.removeFromTop(30); // title bar

        // Left section: algorithm selector + diagram
        auto leftPanel = b.removeFromLeft(200).reduced(6);
        algoLabel.setBounds(leftPanel.removeFromTop(18));
        algoCombo.setBounds(leftPanel.removeFromTop(24));
        leftPanel.removeFromTop(6);
        algoDiagram.setBounds(leftPanel.removeFromTop(90));

        // Left bottom: master + amp env + filter
        leftPanel.removeFromTop(10);
        auto masterRow = leftPanel.removeFromTop(40);
        masterKnob.setBounds(masterRow.removeFromLeft(40));
        masterLabel.setBounds(masterRow.removeFromLeft(50).withY(masterRow.getY() + 12));

        leftPanel.removeFromTop(20);
        auto ampRow = leftPanel.removeFromTop(38);
        ampAtkKnob.setBounds(ampRow.removeFromLeft(36)); ampRow.removeFromLeft(2);
        ampDecKnob.setBounds(ampRow.removeFromLeft(36)); ampRow.removeFromLeft(2);
        ampSusKnob.setBounds(ampRow.removeFromLeft(36)); ampRow.removeFromLeft(2);
        ampRelKnob.setBounds(ampRow.removeFromLeft(36));

        leftPanel.removeFromTop(22); // label space
        auto filtRow = leftPanel.removeFromTop(24);
        filterEnabled.setBounds(filtRow.removeFromLeft(55));
        filterTypeCombo.setBounds(filtRow.removeFromLeft(48));

        leftPanel.removeFromTop(4);
        auto filtKnobRow = leftPanel.removeFromTop(38);
        filterCutoffKnob.setBounds(filtKnobRow.removeFromLeft(38)); filtKnobRow.removeFromLeft(4);
        filterResKnob.setBounds   (filtKnobRow.removeFromLeft(38));

        // Right section: 4 operator panels
        b.reduce(4, 4);
        int opW = b.getWidth() / 4;
        auto opArea = b;
        for (int i = 0; i < 4; ++i) {
            opPanels[i]->setBounds(opArea.removeFromLeft(opW).reduced(2));
        }
    }

private:
    FMSynthProcessor* processor;
    FMLookAndFeel     laf;

    juce::Label     algoLabel;
    juce::ComboBox  algoCombo;
    AlgorithmDiagram algoDiagram;

    std::unique_ptr<FMOperatorPanel> opPanels[4];

    juce::Label    masterLabel;
    juce::Slider   masterKnob;
    juce::Slider   ampAtkKnob, ampDecKnob, ampSusKnob, ampRelKnob;

    juce::ToggleButton filterEnabled;
    juce::ComboBox     filterTypeCombo;
    juce::Slider       filterCutoffKnob, filterResKnob;
};

// ============================================================
// Factory method defined outside the processor class
// ============================================================
inline std::unique_ptr<juce::Component> FMSynthProcessor::createEditor() {
    return std::make_unique<FMSynthComponent>(this);
}
