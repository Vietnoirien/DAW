#pragma once
#include <JuceHeader.h>
#include "KarplusStrongProcessor.h"
#include "../UI/LiBeLookAndFeel.h"

// ─── String visualization ─────────────────────────────────────────────────────
class StringVizComponent : public juce::Component, public juce::Timer {
public:
    void setPlucked(float damping) {
        currentDamping = damping; pluckPhase = 0.0f; startTimerHz(60);
    }
    void timerCallback() override {
        pluckPhase += 0.02f;
        if (pluckPhase > 1.0f) { pluckPhase = 1.0f; stopTimer(); }
        repaint();
    }
    void paint(juce::Graphics& g) override {
        g.fillAll(juce::Colour(0xff04080A));
        g.setColour(juce::Colour(0xff0A2015)); g.drawRect(getLocalBounds(), 1);

        float midY = getHeight() * 0.5f;
        float decay = std::pow(1.0f - currentDamping, 2.0f);
        float amp   = getHeight() * 0.35f * (1.0f - pluckPhase) * decay;

        juce::Path s;
        for (int x = 0; x < getWidth(); ++x) {
            float t = x / (float)getWidth();
            float y = midY + amp * std::sin(t * juce::MathConstants<float>::pi)
                           * std::sin(pluckPhase * 12.0f * juce::MathConstants<float>::pi * t);
            if (x == 0) s.startNewSubPath((float)x, y); else s.lineTo((float)x, y);
        }
        g.setColour(juce::Colour(0xff44DD88).withAlpha(0.85f));
        g.strokePath(s, juce::PathStrokeType(2.0f));
        g.setColour(juce::Colour(0xff335522));
        g.fillRect(0, (int)midY-4, 4, 8); g.fillRect(getWidth()-4, (int)midY-4, 4, 8);
        g.setColour(juce::Colour(0xff44DD88).withAlpha(0.5f));
        g.setFont(juce::FontOptions(9.0f));
        g.drawText("STRING", 5, 2, 80, 11, juce::Justification::centredLeft);
    }
private:
    float currentDamping{0.5f}, pluckPhase{1.0f};
};

// ─── KarplusStrongComponent ───────────────────────────────────────────────────
// Layout (320px tall):
//  ┌────────────────────────────────────────────────────────┐
//  │  KARPLUS-STRONG  |  PHYSICAL MODELING       [title]   │
//  ├────────────────┬───────────────────────────────────────┤
//  │ [String viz  ] │  EXCITATION                          │
//  │                │  [en▾] [Noise Burst▾]               │
//  │                │  [Lvl][Damp][Stretch][Pickup][Decay] │
//  │                │  ─────────────────────────────────   │
//  │                │  AMP                                 │
//  │                │  [Atk][Rel][Master]                  │
//  └────────────────┴───────────────────────────────────────┘
class KarplusStrongComponent : public juce::Component {
public:
    explicit KarplusStrongComponent(KarplusStrongProcessor* proc) : processor(proc), laf(juce::Colour(0xff44DD88)) {
        setLookAndFeel(&laf);
        addAndMakeVisible(stringViz);

        addAndMakeVisible(exTypeCombo);
        exTypeCombo.addItem("Noise Burst", 1); exTypeCombo.addItem("Impulse", 2);
        exTypeCombo.addItem("Filtered Noise", 3);
        exTypeCombo.setSelectedId(proc->params.excitationType.load()+1, juce::dontSendNotification);
        exTypeCombo.onChange = [this] { processor->params.excitationType.store(exTypeCombo.getSelectedId()-1); };

        auto& p = proc->params;
        auto sk = [&](LiBeKnob& k, const char* lbl, double v, double mn, double mx, double dv, const char* pid) {
            k.setup(lbl, v, mn, mx, dv, pid, juce::Colour(0xff44DD88)); addAndMakeVisible(k);
        };
        sk(kLvl,    "Level",   p.excitationLevel.load(), 0.0,   1.0,  1.0,   "KS/ExcitationLevel");
        sk(kDamp,   "Damp",    p.damping.load(),          0.0,   1.0,  0.5,   "KS/Damping");
        sk(kStr,    "Stretch", p.stretch.load(),          1.0,   2.0,  1.0,   "KS/Stretch");
        sk(kPickup, "Pickup",  p.pickupPos.load(),        0.0,   1.0,  0.5,   "KS/Pickup");
        sk(kDecay,  "Decay",   p.decayTime.load(),        0.1,   5.0,  1.0,   "KS/DecayTime");
        sk(kAtk,    "Atk",     p.attack.load(),           0.001, 0.5,  0.001, "KS/Attack");
        sk(kRel,    "Rel",     p.release.load(),          0.001, 2.0,  0.1,   "KS/Release");
        sk(kMast,   "Master",  p.masterLevel.load(),      0.0,   1.0,  0.8,   "KS/MasterLevel");

        kLvl.slider.onValueChange    = [&p,this] { p.excitationLevel.store((float)kLvl.slider.getValue()); };
        kDamp.slider.onValueChange   = [&p,this] { p.damping.store        ((float)kDamp.slider.getValue()); };
        kStr.slider.onValueChange    = [&p,this] { p.stretch.store        ((float)kStr.slider.getValue()); };
        kPickup.slider.onValueChange = [&p,this] { p.pickupPos.store      ((float)kPickup.slider.getValue()); };
        kDecay.slider.onValueChange  = [&p,this] { p.decayTime.store      ((float)kDecay.slider.getValue()); };
        kAtk.slider.onValueChange    = [&p,this] { p.attack.store         ((float)kAtk.slider.getValue()); };
        kRel.slider.onValueChange    = [&p,this] { p.release.store        ((float)kRel.slider.getValue()); };
        kMast.slider.onValueChange   = [&p,this] { p.masterLevel.store    ((float)kMast.slider.getValue()); };

        setSize(680, 320);
    }
    ~KarplusStrongComponent() override { setLookAndFeel(nullptr); }

    void paint(juce::Graphics& g) override {
        g.fillAll(juce::Colour(0xff07070E));
        g.setColour(juce::Colour(0xff0A1A0A)); g.fillRect(0, 0, getWidth(), kTH);
        g.setColour(juce::Colour(0xff44DD88)); g.setFont(juce::FontOptions(13.0f, juce::Font::bold));
        g.drawText("KARPLUS-STRONG  |  PHYSICAL MODELING", 12, 0, getWidth()-12, kTH, juce::Justification::centredLeft);
        g.setColour(juce::Colour(0xff202030));
        g.drawHorizontalLine(kTH, 0.0f, (float)getWidth());
        g.drawVerticalLine(kLeftW, (float)kTH, (float)getHeight());

        int rx = kLeftW + 10;
        auto sl = [&](const char* t, int x, int y) {
            g.setColour(juce::Colour(0xff44DD88).withAlpha(0.65f));
            g.setFont(juce::FontOptions(9.0f, juce::Font::bold));
            g.drawText(t, x, y, 160, 12, juce::Justification::centredLeft);
        };
        sl("EXCITATION", rx, exTypeCombo.getY() - 13);
        sl("AMP",        rx, kAtk.getY() - 13);
    }

    void resized() override {
        auto b = getLocalBounds().withTrimmedTop(kTH);
        stringViz.setBounds(b.removeFromLeft(kLeftW).reduced(6));

        auto right = b.reduced(8, 3);
        right.removeFromTop(14);
        exTypeCombo.setBounds(right.removeFromTop(22).removeFromLeft(160));
        right.removeFromTop(4);
        int x = right.getX(), y = right.getY();
        for (auto* k : std::initializer_list<LiBeKnob*>{ &kLvl, &kDamp, &kStr, &kPickup, &kDecay }) {
            k->setBounds(x, y, LiBeKnob::kW, LiBeKnob::kH); x += LiBeKnob::kW + 4;
        }
        right.removeFromTop(LiBeKnob::kH + 14);
        x = right.getX(); y = right.getY();
        for (auto* k : std::initializer_list<LiBeKnob*>{ &kAtk, &kRel, &kMast }) {
            k->setBounds(x, y, LiBeKnob::kW, LiBeKnob::kH); x += LiBeKnob::kW + 4;
        }
    }

private:
    static constexpr int kTH = 30, kLeftW = 200;
    KarplusStrongProcessor* processor;
    LiBeLookAndFeel laf;
    StringVizComponent stringViz;
    juce::ComboBox exTypeCombo;
    LiBeKnob kLvl, kDamp, kStr, kPickup, kDecay, kAtk, kRel, kMast;
};

inline std::unique_ptr<juce::Component> KarplusStrongProcessor::createEditor() {
    return std::make_unique<KarplusStrongComponent>(this);
}
