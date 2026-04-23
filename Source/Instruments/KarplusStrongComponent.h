#pragma once
#include <JuceHeader.h>
#include "KarplusStrongProcessor.h"

// ============================================================
// KS Look and Feel (green/wood theme)
// ============================================================
class KSLookAndFeel : public juce::LookAndFeel_V4 {
public:
    KSLookAndFeel() {
        setColour(juce::Slider::rotarySliderFillColourId,    juce::Colour(0xff44DD88));
        setColour(juce::Slider::rotarySliderOutlineColourId, juce::Colour(0xff0A2015));
        setColour(juce::Slider::thumbColourId,               juce::Colour(0xff99FFCC));
        setColour(juce::ComboBox::backgroundColourId,        juce::Colour(0xff081208));
        setColour(juce::ComboBox::textColourId,              juce::Colour(0xff44DD88));
        setColour(juce::ComboBox::outlineColourId,           juce::Colour(0xff0A3020));
    }
    void drawRotarySlider(juce::Graphics& g, int x, int y, int w, int h,
                          float pos, float sa, float ea, juce::Slider&) override {
        float cx=x+w*0.5f, cy=y+h*0.5f, r=(float)juce::jmin(w/2,h/2)-3.0f;
        float angle=sa+pos*(ea-sa);
        juce::Path arc; arc.addCentredArc(cx,cy,r,r,0,sa,ea,true);
        g.setColour(juce::Colour(0xff0A2015)); g.strokePath(arc,juce::PathStrokeType(3));
        juce::Path fill; fill.addCentredArc(cx,cy,r,r,0,sa,angle,true);
        g.setColour(juce::Colour(0xff44DD88)); g.strokePath(fill,juce::PathStrokeType(3));
        g.setColour(juce::Colour(0xff081208));
        g.fillEllipse(cx-r*0.6f,cy-r*0.6f,r*1.2f,r*1.2f);
        juce::Path p; p.addRectangle(-1.5f,-r*0.5f,3.0f,r*0.5f);
        p.applyTransform(juce::AffineTransform::rotation(angle).translated(cx,cy));
        g.setColour(juce::Colour(0xff99FFCC)); g.fillPath(p);
    }
};

// ============================================================
// String vibration visualization
// ============================================================
class StringVizComponent : public juce::Component, public juce::Timer {
public:
    void setPlucked(float damping) {
        currentDamping = damping;
        pluckPhase = 0.0f;
        startTimerHz(60);
    }
    void timerCallback() override {
        pluckPhase += 0.02f;
        if (pluckPhase > 1.0f) { pluckPhase = 1.0f; stopTimer(); }
        repaint();
    }
    void paint(juce::Graphics& g) override {
        g.fillAll(juce::Colour(0xff030A05));
        g.setColour(juce::Colour(0xff0A2015)); g.drawRect(getLocalBounds(),1);

        float midY = getHeight() * 0.5f;
        float decay = std::pow(1.0f - currentDamping, 2.0f);
        float amp   = getHeight() * 0.35f * (1.0f - pluckPhase) * decay;

        juce::Path string;
        for (int x = 0; x < getWidth(); ++x) {
            float t  = x / (float)getWidth();
            float y  = midY + amp * std::sin(t * juce::MathConstants<float>::pi)
                            * std::sin(pluckPhase * 12.0f * juce::MathConstants<float>::pi * t);
            if (x == 0) string.startNewSubPath((float)x, y);
            else         string.lineTo((float)x, y);
        }

        // String body
        g.setColour(juce::Colour(0xff44DD88).withAlpha(0.8f));
        g.strokePath(string, juce::PathStrokeType(2.0f));

        // Bridge markers
        g.setColour(juce::Colour(0xff335522));
        g.fillRect(0, (int)midY - 4, 4, 8);
        g.fillRect(getWidth() - 4, (int)midY - 4, 4, 8);

        g.setColour(juce::Colour(0xff44DD88).withAlpha(0.5f));
        g.setFont(juce::FontOptions(9.0f));
        g.drawText("KARPLUS-STRONG", 4, 2, 140, 12, juce::Justification::centredLeft);
    }
private:
    float currentDamping { 0.5f };
    float pluckPhase     { 1.0f };
};

// ============================================================
// KarplusStrongComponent
// ============================================================
class KarplusStrongComponent : public juce::Component {
public:
    explicit KarplusStrongComponent(KarplusStrongProcessor* proc) : processor(proc) {
        setLookAndFeel(&laf);
        addAndMakeVisible(stringViz);

        // Excitation type
        addAndMakeVisible(exTypeLabel);
        exTypeLabel.setText("Excitation", juce::dontSendNotification);
        exTypeLabel.setColour(juce::Label::textColourId, juce::Colour(0xff44DD88));
        exTypeLabel.setFont(juce::FontOptions(11.0f, juce::Font::bold));

        addAndMakeVisible(exTypeCombo);
        exTypeCombo.addItem("Noise Burst",    1);
        exTypeCombo.addItem("Impulse",        2);
        exTypeCombo.addItem("Filtered Noise", 3);
        exTypeCombo.setSelectedId(proc->params.excitationType.load()+1, juce::dontSendNotification);
        exTypeCombo.onChange=[this]{
            processor->params.excitationType.store(exTypeCombo.getSelectedId()-1);
        };

        auto sk=[&](juce::Slider& s,double v,double mn,double mx,double def=-1){
            addAndMakeVisible(s);
            s.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
            s.setTextBoxStyle(juce::Slider::NoTextBox,false,0,0);
            s.setRange(mn,mx); s.setValue(v,juce::dontSendNotification);
            s.setDoubleClickReturnValue(true,def<0?v:def);
        };
        auto& p=proc->params;
        sk(exLevelKnob, p.excitationLevel.load(), 0, 1, 1);
        sk(dampingKnob, p.damping.load(),          0, 1, 0.5);
        sk(stretchKnob, p.stretch.load(),          1, 2, 1);
        sk(pickupKnob,  p.pickupPos.load(),         0, 1, 0.5);
        sk(decayKnob,   p.decayTime.load(),         0.1, 5, 1);
        sk(masterKnob,  p.masterLevel.load(),       0, 1, 0.8);
        sk(attackKnob,  p.attack.load(),            0.001, 0.5, 0.001);
        sk(releaseKnob, p.release.load(),           0.001, 2, 0.1);

        exLevelKnob.onValueChange =[&]{p.excitationLevel.store((float)exLevelKnob.getValue());};
        dampingKnob.onValueChange =[&]{p.damping.store((float)dampingKnob.getValue());};
        stretchKnob.onValueChange =[&]{p.stretch.store((float)stretchKnob.getValue());};
        pickupKnob.onValueChange  =[&]{p.pickupPos.store((float)pickupKnob.getValue());};
        decayKnob.onValueChange   =[&]{p.decayTime.store((float)decayKnob.getValue());};
        masterKnob.onValueChange  =[&]{p.masterLevel.store((float)masterKnob.getValue());};
        attackKnob.onValueChange  =[&]{p.attack.store((float)attackKnob.getValue());};
        releaseKnob.onValueChange =[&]{p.release.store((float)releaseKnob.getValue());};

        setSize(520, 280);
    }
    ~KarplusStrongComponent() override { setLookAndFeel(nullptr); }

    void paint(juce::Graphics& g) override {
        g.fillAll(juce::Colour(0xff030A05));
        g.setColour(juce::Colour(0xff0A1A0A)); g.fillRect(0,0,getWidth(),30);
        g.setColour(juce::Colour(0xff44DD88));
        g.setFont(juce::FontOptions(14.0f,juce::Font::bold));
        g.drawText("KARPLUS-STRONG  |  PHYSICAL MODELING",10,0,getWidth()-10,30,juce::Justification::centredLeft);
        g.setColour(juce::Colour(0xff0A2015)); g.drawHorizontalLine(30,0.0f,(float)getWidth());

        g.setColour(juce::Colours::grey); g.setFont(juce::FontOptions(9.0f));
        struct{juce::Slider*s;const char*l;}lbs[]={
            {&exLevelKnob,"Level"},{&dampingKnob,"Damp"},{&stretchKnob,"Stretch"},
            {&pickupKnob,"Pickup"},{&decayKnob,"Decay"},{&masterKnob,"Master"},
            {&attackKnob,"Atk"},{&releaseKnob,"Rel"}
        };
        for(auto& l:lbs)
            g.drawText(l.l,l.s->getX(),l.s->getBottom(),l.s->getWidth(),11,juce::Justification::centred);

        g.setColour(juce::Colour(0xff44DD88).withAlpha(0.6f));
        g.setFont(juce::FontOptions(10.0f,juce::Font::bold));
        g.drawText("PLUCK MODEL",exLevelKnob.getX()-2,exLevelKnob.getY()-16,100,14,juce::Justification::centredLeft);
        g.drawText("AMP",attackKnob.getX()-2,attackKnob.getY()-16,60,14,juce::Justification::centredLeft);
    }

    void resized() override {
        auto b=getLocalBounds(); b.removeFromTop(30);
        stringViz.setBounds(b.removeFromLeft(200).reduced(6));
        b.reduce(4,4);
        int kw=40;
        auto topRow=b.removeFromTop(26);
        exTypeLabel.setBounds(topRow.removeFromLeft(70));
        exTypeCombo.setBounds(topRow.removeFromLeft(130));

        b.removeFromTop(22);
        auto r1=b.removeFromTop(kw);
        exLevelKnob.setBounds(r1.removeFromLeft(kw)); r1.removeFromLeft(6);
        dampingKnob.setBounds(r1.removeFromLeft(kw)); r1.removeFromLeft(6);
        stretchKnob.setBounds(r1.removeFromLeft(kw)); r1.removeFromLeft(6);
        pickupKnob.setBounds (r1.removeFromLeft(kw)); r1.removeFromLeft(6);
        decayKnob.setBounds  (r1.removeFromLeft(kw)); r1.removeFromLeft(6);
        masterKnob.setBounds (r1.removeFromLeft(kw));

        b.removeFromTop(22);
        auto r2=b.removeFromTop(kw);
        attackKnob.setBounds (r2.removeFromLeft(kw)); r2.removeFromLeft(6);
        releaseKnob.setBounds(r2.removeFromLeft(kw));
    }

private:
    KarplusStrongProcessor* processor;
    KSLookAndFeel laf;
    StringVizComponent stringViz;
    juce::Label    exTypeLabel;
    juce::ComboBox exTypeCombo;
    juce::Slider   exLevelKnob, dampingKnob, stretchKnob, pickupKnob, decayKnob;
    juce::Slider   masterKnob, attackKnob, releaseKnob;
};

inline std::unique_ptr<juce::Component> KarplusStrongProcessor::createEditor() {
    return std::make_unique<KarplusStrongComponent>(this);
}
