#pragma once
#include <JuceHeader.h>
#include "OscProcessor.h"

class OscLookAndFeel : public juce::LookAndFeel_V4 {
public:
    OscLookAndFeel() {
        setColour(juce::Slider::rotarySliderFillColourId, juce::Colour(0xff00d0ff));
        setColour(juce::Slider::rotarySliderOutlineColourId, juce::Colour(0xff252540));
        setColour(juce::Slider::thumbColourId, juce::Colours::white);
    }
    
    void drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height, float sliderPos,
                          const float rotaryStartAngle, const float rotaryEndAngle, juce::Slider& slider) override {
        auto radius = (float) juce::jmin(width / 2, height / 2) - 4.0f;
        auto centreX = (float) x + (float) width  * 0.5f;
        auto centreY = (float) y + (float) height * 0.5f;
        auto rx = centreX - radius;
        auto ry = centreY - radius;
        auto rw = radius * 2.0f;
        auto angle = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);
        
        g.setColour(findColour(juce::Slider::rotarySliderOutlineColourId));
        g.fillEllipse(rx, ry, rw, rw);
        
        g.setColour(juce::Colour(0xff121220));
        g.fillEllipse(rx + 2.0f, ry + 2.0f, rw - 4.0f, rw - 4.0f);
        
        juce::Path p;
        auto pointerLength = radius * 0.8f;
        auto pointerThickness = 3.0f;
        p.addRectangle(-pointerThickness * 0.5f, -radius, pointerThickness, pointerLength);
        p.applyTransform(juce::AffineTransform::rotation(angle).translated(centreX, centreY));
        
        g.setColour(findColour(juce::Slider::rotarySliderFillColourId));
        g.fillPath(p);
    }
};

class OscPanel : public juce::Component {
public:
    OscPanel(const juce::String& name, OscParams::Osc& params) : oscName(name), oscParams(params) {
        addAndMakeVisible(enabledToggle);
        enabledToggle.setButtonText(name);
        enabledToggle.setToggleState(params.enabled.load(), juce::dontSendNotification);
        enabledToggle.onClick = [this] { oscParams.enabled.store(enabledToggle.getToggleState()); };
        
        addAndMakeVisible(waveformCombo);
        waveformCombo.addItem("Sine", 1);
        waveformCombo.addItem("Tri", 2);
        waveformCombo.addItem("Saw", 3);
        waveformCombo.addItem("Square", 4);
        waveformCombo.addItem("WT", 5);
        waveformCombo.setSelectedId(params.waveform.load() + 1, juce::dontSendNotification);
        waveformCombo.onChange = [this] { oscParams.waveform.store(waveformCombo.getSelectedId() - 1); };
        
        setupSlider(octaveKnob, params.octave.load(), -2.0, 2.0, 1.0);
        setupSlider(coarseKnob, params.coarse.load(), -24.0, 24.0, 1.0);
        setupSlider(unisonKnob, params.unisonVoices.load(), 1, 8, 1);
        setupSlider(detuneKnob, params.unisonDetune.load(), 0.0, 1.0, 0.01);
        setupSlider(levelKnob, params.level.load(), 0.0, 1.0, 0.01);
        
        octaveKnob.onValueChange = [this] { oscParams.octave.store(octaveKnob.getValue()); };
        coarseKnob.onValueChange = [this] { oscParams.coarse.store(coarseKnob.getValue()); };
        unisonKnob.onValueChange = [this] { oscParams.unisonVoices.store((int)unisonKnob.getValue()); };
        detuneKnob.onValueChange = [this] { oscParams.unisonDetune.store(detuneKnob.getValue()); };
        levelKnob.onValueChange = [this] { oscParams.level.store(levelKnob.getValue()); };
    }
    
    void paint(juce::Graphics& g) override {
        g.fillAll(juce::Colour(0xff151525));
        g.setColour(juce::Colour(0xff252540));
        g.drawRect(getLocalBounds(), 1);
        
        g.setColour(juce::Colours::grey);
        g.setFont(10.0f);
        g.drawText("Oct", octaveKnob.getX(), octaveKnob.getBottom(), octaveKnob.getWidth(), 12, juce::Justification::centred);
        g.drawText("Crs", coarseKnob.getX(), coarseKnob.getBottom(), coarseKnob.getWidth(), 12, juce::Justification::centred);
        g.drawText("Unis", unisonKnob.getX(), unisonKnob.getBottom(), unisonKnob.getWidth(), 12, juce::Justification::centred);
        g.drawText("Det", detuneKnob.getX(), detuneKnob.getBottom(), detuneKnob.getWidth(), 12, juce::Justification::centred);
        g.drawText("Level", levelKnob.getX(), levelKnob.getBottom(), levelKnob.getWidth(), 12, juce::Justification::centred);
    }
    
    void resized() override {
        auto b = getLocalBounds().reduced(5);
        auto topRow = b.removeFromTop(24);
        enabledToggle.setBounds(topRow.removeFromLeft(60));
        waveformCombo.setBounds(topRow.removeFromLeft(80));
        
        b.removeFromTop(10);
        int w = 36;
        int spacing = 8;
        octaveKnob.setBounds(b.removeFromLeft(w).withHeight(w)); b.removeFromLeft(spacing);
        coarseKnob.setBounds(b.removeFromLeft(w).withHeight(w)); b.removeFromLeft(spacing);
        unisonKnob.setBounds(b.removeFromLeft(w).withHeight(w)); b.removeFromLeft(spacing);
        detuneKnob.setBounds(b.removeFromLeft(w).withHeight(w)); b.removeFromLeft(spacing);
        levelKnob.setBounds(b.removeFromLeft(w).withHeight(w));
    }
    
private:
    void setupSlider(juce::Slider& s, double val, double min, double max, double inc) {
        addAndMakeVisible(s);
        s.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        s.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        s.setRange(min, max, inc);
        s.setValue(val, juce::dontSendNotification);
        s.setDoubleClickReturnValue(true, val);
    }

    juce::String oscName;
    OscParams::Osc& oscParams;
    juce::ToggleButton enabledToggle;
    juce::ComboBox waveformCombo;
    juce::Slider octaveKnob, coarseKnob, unisonKnob, detuneKnob, levelKnob;
};

class FilterPanel : public juce::Component {
public:
    FilterPanel(OscParams::Filter& params) : filterParams(params) {
        addAndMakeVisible(enabledToggle);
        enabledToggle.setButtonText("Filter");
        enabledToggle.setToggleState(params.enabled.load(), juce::dontSendNotification);
        enabledToggle.onClick = [this] { filterParams.enabled.store(enabledToggle.getToggleState()); };
        
        addAndMakeVisible(typeCombo);
        typeCombo.addItem("LP", 1);
        typeCombo.addItem("HP", 2);
        typeCombo.addItem("BP", 3);
        typeCombo.setSelectedId(params.type.load() + 1, juce::dontSendNotification);
        typeCombo.onChange = [this] { filterParams.type.store(typeCombo.getSelectedId() - 1); };

        auto setup = [&](juce::Slider& s, double v, double min, double max) {
            addAndMakeVisible(s);
            s.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
            s.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
            s.setRange(min, max);
            s.setValue(v, juce::dontSendNotification);
            s.setDoubleClickReturnValue(true, v);
        };
        
        setup(cutoffKnob, params.cutoff.load(), 20.0, 20000.0);
        cutoffKnob.setSkewFactorFromMidPoint(1000.0);
        setup(resKnob, params.resonance.load(), 0.1, 10.0);
        setup(envAmtKnob, params.envAmount.load(), -1.0, 1.0);
        
        cutoffKnob.onValueChange = [this] { filterParams.cutoff.store(cutoffKnob.getValue()); };
        resKnob.onValueChange = [this] { filterParams.resonance.store(resKnob.getValue()); };
        envAmtKnob.onValueChange = [this] { filterParams.envAmount.store(envAmtKnob.getValue()); };
    }
    
    void paint(juce::Graphics& g) override {
        g.fillAll(juce::Colour(0xff151525));
        g.setColour(juce::Colour(0xff252540));
        g.drawRect(getLocalBounds(), 1);
        
        g.setColour(juce::Colours::grey);
        g.setFont(10.0f);
        g.drawText("Cutoff", cutoffKnob.getX(), cutoffKnob.getBottom(), cutoffKnob.getWidth(), 12, juce::Justification::centred);
        g.drawText("Res", resKnob.getX(), resKnob.getBottom(), resKnob.getWidth(), 12, juce::Justification::centred);
        g.drawText("Env", envAmtKnob.getX(), envAmtKnob.getBottom(), envAmtKnob.getWidth(), 12, juce::Justification::centred);
    }
    
    void resized() override {
        auto b = getLocalBounds().reduced(5);
        auto topRow = b.removeFromTop(24);
        enabledToggle.setBounds(topRow.removeFromLeft(60));
        typeCombo.setBounds(topRow.removeFromLeft(60));
        
        b.removeFromTop(10);
        int w = 40; int spacing = 15;
        cutoffKnob.setBounds(b.removeFromLeft(w).withHeight(w)); b.removeFromLeft(spacing);
        resKnob.setBounds(b.removeFromLeft(w).withHeight(w)); b.removeFromLeft(spacing);
        envAmtKnob.setBounds(b.removeFromLeft(w).withHeight(w));
    }
private:
    OscParams::Filter& filterParams;
    juce::ToggleButton enabledToggle;
    juce::ComboBox typeCombo;
    juce::Slider cutoffKnob, resKnob, envAmtKnob;
};

class EnvPanel : public juce::Component {
public:
    EnvPanel(const juce::String& name, OscParams::ADSRNode& params) : envName(name), envParams(params) {
        auto setup = [&](juce::Slider& s, double v, double min, double max) {
            addAndMakeVisible(s);
            s.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
            s.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
            s.setRange(min, max);
            s.setValue(v, juce::dontSendNotification);
            s.setDoubleClickReturnValue(true, v);
        };
        setup(attKnob, params.attack.load(), 0.001, 5.0);
        setup(decKnob, params.decay.load(), 0.001, 5.0);
        setup(susKnob, params.sustain.load(), 0.0, 1.0);
        setup(relKnob, params.release.load(), 0.001, 5.0);
        
        attKnob.setSkewFactorFromMidPoint(0.5);
        decKnob.setSkewFactorFromMidPoint(0.5);
        relKnob.setSkewFactorFromMidPoint(0.5);
        
        attKnob.onValueChange = [this] { envParams.attack.store(attKnob.getValue()); };
        decKnob.onValueChange = [this] { envParams.decay.store(decKnob.getValue()); };
        susKnob.onValueChange = [this] { envParams.sustain.store(susKnob.getValue()); };
        relKnob.onValueChange = [this] { envParams.release.store(relKnob.getValue()); };
    }
    
    void paint(juce::Graphics& g) override {
        g.fillAll(juce::Colour(0xff151525));
        g.setColour(juce::Colour(0xff252540));
        g.drawRect(getLocalBounds(), 1);
        
        g.setColour(juce::Colours::white);
        g.setFont(12.0f);
        g.drawText(envName, 5, 5, 100, 15, juce::Justification::centredLeft);
        
        g.setColour(juce::Colours::grey);
        g.setFont(10.0f);
        g.drawText("A", attKnob.getX(), attKnob.getBottom(), attKnob.getWidth(), 12, juce::Justification::centred);
        g.drawText("D", decKnob.getX(), decKnob.getBottom(), decKnob.getWidth(), 12, juce::Justification::centred);
        g.drawText("S", susKnob.getX(), susKnob.getBottom(), susKnob.getWidth(), 12, juce::Justification::centred);
        g.drawText("R", relKnob.getX(), relKnob.getBottom(), relKnob.getWidth(), 12, juce::Justification::centred);
    }
    
    void resized() override {
        auto b = getLocalBounds().reduced(5);
        b.removeFromTop(20);
        int w = 36; int spacing = 10;
        attKnob.setBounds(b.removeFromLeft(w).withHeight(w)); b.removeFromLeft(spacing);
        decKnob.setBounds(b.removeFromLeft(w).withHeight(w)); b.removeFromLeft(spacing);
        susKnob.setBounds(b.removeFromLeft(w).withHeight(w)); b.removeFromLeft(spacing);
        relKnob.setBounds(b.removeFromLeft(w).withHeight(w));
    }
private:
    juce::String envName;
    OscParams::ADSRNode& envParams;
    juce::Slider attKnob, decKnob, susKnob, relKnob;
};

class OscContentComponent : public juce::Component {
public:
    OscContentComponent(OscProcessor* proc) : processor(proc),
        oscA("OSC A", proc->params.oscA),
        oscB("OSC B", proc->params.oscB),
        oscC("OSC C", proc->params.oscC),
        filter(proc->params.filter),
        ampEnv("Amp Env", proc->params.ampEnv),
        filtEnv("Filter Env", proc->params.filterEnv)
    {
        addAndMakeVisible(oscA);
        addAndMakeVisible(oscB);
        addAndMakeVisible(oscC);
        addAndMakeVisible(filter);
        addAndMakeVisible(ampEnv);
        addAndMakeVisible(filtEnv);
    }

    void paint(juce::Graphics& g) override {
        g.fillAll(juce::Colour(0xff0d0d1a));
    }

    void resized() override {
        int x = 5;
        int y = 5;
        int colWidth = 0;
        int maxH = getLocalBounds().getHeight();
        
        struct Item { juce::Component* c; int w; int h; };
        Item items[] = {
            { &oscA, 240, 100 },
            { &oscB, 240, 100 },
            { &oscC, 240, 100 },
            { &filter, 200, 100 },
            { &ampEnv, 200, 100 },
            { &filtEnv, 200, 100 }
        };
        
        for (auto& item : items) {
            if (y + item.h > maxH && y > 5) {
                x += colWidth + 5;
                y = 5;
                colWidth = 0;
            }
            item.c->setBounds(x, y, item.w, item.h);
            colWidth = std::max(colWidth, item.w);
            y += item.h + 5;
        }
    }

private:
    OscProcessor* processor;
    OscPanel oscA, oscB, oscC;
    FilterPanel filter;
    EnvPanel ampEnv, filtEnv;
};

class OscComponent : public juce::Component {
public:
    OscComponent(OscProcessor* proc) : content(proc) {
        setLookAndFeel(&laf);
        addAndMakeVisible(viewport);
        viewport.setViewedComponent(&content, false);
        viewport.setScrollBarsShown(false, true, false, false);
    }
    
    ~OscComponent() override {
        setLookAndFeel(nullptr);
    }

    void paint(juce::Graphics& g) override {
        g.fillAll(juce::Colour(0xff0d0d1a));
    }

    void resized() override {
        viewport.setBounds(getLocalBounds());
        int h = std::max(120, getLocalBounds().getHeight() - viewport.getScrollBarThickness());
        
        int x = 5, y = 5, colW = 0;
        int maxH = h;
        struct Item { int w; int h; };
        Item items[] = { {240,100}, {240,100}, {240,100}, {200,100}, {200,100}, {200,100} };
        
        for (auto& i : items) {
            if (y + i.h > maxH && y > 5) { x += colW + 5; y = 5; colW = 0; }
            colW = std::max(colW, i.w);
            y += i.h + 5;
        }
        
        content.setSize(x + colW + 5, h);
    }

private:
    OscLookAndFeel laf;
    OscContentComponent content;
    juce::Viewport viewport;
};
