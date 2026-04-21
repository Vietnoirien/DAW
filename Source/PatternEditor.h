#pragma once
#include <JuceHeader.h>
#include <cmath>
#include "ClipData.h"

class EuclideanCircleViewer : public juce::Component {
public:
    EuclideanCircleViewer() { activeSteps.assign(steps, 0); } // must match default steps=16

    void setParams(int k, int n) {
        pulses = k;
        steps = std::max(1, n);
        activeSteps.assign(steps, 0);

        for (int i = 0; i < steps; ++i) {
            activeSteps[i] = ((i * pulses) % steps < pulses) ? 1 : 0;
        }
        repaint();
    }

    void setPlayheadPhase(float phase) {
        if (playheadPhase != phase) {
            playheadPhase = phase;
            repaint();
        }
    }

    void rotateMap(int dir) {
        if (activeSteps.empty()) return;
        if (dir > 0) {
            std::rotate(activeSteps.rbegin(), activeSteps.rbegin() + 1, activeSteps.rend());
        } else {
            std::rotate(activeSteps.begin(), activeSteps.begin() + 1, activeSteps.end());
        }
        repaint();
        if (onHitMapToggled) onHitMapToggled(activeSteps);
    }

    void paint(juce::Graphics& g) override {
        g.fillAll(juce::Colours::black);
        auto rawBounds = getLocalBounds().toFloat();
        if (rawBounds.isEmpty()) return;

        float diameter = std::max(10.0f, std::min(rawBounds.getWidth(), rawBounds.getHeight()) - 60.0f);
        auto bounds = juce::Rectangle<float>(0, 0, diameter, diameter).withCentre(rawBounds.getCentre());

        g.setColour(juce::Colours::darkgrey);
        g.drawEllipse(bounds, 2.0f);

        auto center = bounds.getCentre();
        float radius = diameter * 0.5f;

        int activeIndex = -1;
        if (playheadPhase >= 0.0f) {
            activeIndex = static_cast<int>(playheadPhase * steps);
            if (activeIndex >= steps) activeIndex = steps - 1;
        }

        for (int i = 0; i < steps; ++i) {
            bool hit = activeSteps[i] != 0;
            bool isActiveStep = (i == activeIndex);

            float angle = juce::MathConstants<float>::twoPi * (float)i / (float)steps - juce::MathConstants<float>::halfPi;
            juce::Point<float> p (center.x + std::cos(angle) * radius, center.y + std::sin(angle) * radius);
            
            if (isActiveStep) {
                if (hit) {
                    g.setColour(juce::Colours::lime);
                    g.fillEllipse(p.x - 12.0f, p.y - 12.0f, 24.0f, 24.0f);
                } else {
                    g.setColour(juce::Colours::white);
                    g.drawEllipse(p.x - 10.0f, p.y - 10.0f, 20.0f, 20.0f, 2.0f);
                }
            } else {
                g.setColour(hit ? juce::Colours::lightgreen : juce::Colours::darkcyan.darker());
                g.fillEllipse(p.x - 8.0f, p.y - 8.0f, 16.0f, 16.0f);
            }

            // Numerotation (draw just outside the circle)
            juce::Point<float> numP (center.x + std::cos(angle) * (radius + 20.0f), center.y + std::sin(angle) * (radius + 20.0f));
            g.setColour(isActiveStep ? juce::Colours::white : juce::Colours::lightgrey);
            g.setFont(juce::Font(12.0f, isActiveStep ? juce::Font::bold : juce::Font::plain));
            g.drawText(juce::String(i + 1), numP.x - 10.0f, numP.y - 10.0f, 20.0f, 20.0f, juce::Justification::centred);
        }

        // Draw rotate arrows
        juce::Rectangle<float> leftBtn(bounds.getX() - 40.0f, bounds.getCentreY() - 15.0f, 30.0f, 30.0f);
        juce::Rectangle<float> rightBtn(bounds.getRight() + 10.0f, bounds.getCentreY() - 15.0f, 30.0f, 30.0f);
        
        g.setColour(juce::Colours::white.withAlpha(0.6f));
        g.setFont(juce::Font(24.0f, juce::Font::bold));
        g.drawText("<", leftBtn, juce::Justification::centred);
        g.drawText(">", rightBtn, juce::Justification::centred);
    }

    void mouseDown(const juce::MouseEvent& e) override {
        auto rawBounds = getLocalBounds().toFloat();
        if (rawBounds.isEmpty()) return;
        
        float diameter = std::max(10.0f, std::min(rawBounds.getWidth(), rawBounds.getHeight()) - 60.0f);
        auto bounds = juce::Rectangle<float>(0, 0, diameter, diameter).withCentre(rawBounds.getCentre());
        auto center = bounds.getCentre();
        float radius = diameter * 0.5f;

        juce::Rectangle<float> leftBtn(bounds.getX() - 40.0f, bounds.getCentreY() - 15.0f, 30.0f, 30.0f);
        juce::Rectangle<float> rightBtn(bounds.getRight() + 10.0f, bounds.getCentreY() - 15.0f, 30.0f, 30.0f);
        if (leftBtn.contains(e.position)) { rotateMap(-1); return; }
        if (rightBtn.contains(e.position)) { rotateMap(1); return; }

        for (int i = 0; i < steps; ++i) {
            float angle = juce::MathConstants<float>::twoPi * (float)i / (float)steps - juce::MathConstants<float>::halfPi;
            juce::Point<float> p (center.x + std::cos(angle) * radius, center.y + std::sin(angle) * radius);
            
            if (e.position.getDistanceFrom(p) <= 12.0f) {
                activeSteps[i] = activeSteps[i] == 0 ? 1 : 0;
                repaint();
                if (onHitMapToggled) onHitMapToggled(activeSteps);
                break;
            }
        }
    }

    std::function<void(const std::vector<uint8_t>&)> onHitMapToggled;

    int pulses = 0;
    int steps = 16;
    std::vector<uint8_t> activeSteps;
    float playheadPhase = -1.0f;
};

class PianoRollViewer : public juce::Component {
public:
    PianoRollViewer() {}

    void paint(juce::Graphics& g) override {
        g.fillAll(juce::Colours::black);
        g.setColour(juce::Colours::darkgrey.darker());
        for (int i = 0; i < 32; ++i) {
            g.drawLine((float)i * 30.0f, 0, (float)i * 30.0f, (float)getHeight());
        }
        for (int i = 0; i < 16; ++i) {
            g.drawLine(0, (float)i * 20.0f, (float)getWidth(), (float)i * 20.0f);
        }
        g.setColour(juce::Colours::lightgrey);
        g.drawText("Mode A: Virtual Piano Roll (Drag to draw notes)", getLocalBounds(), juce::Justification::centred);
    }
};

class PatternEditor : public juce::Component, public juce::Slider::Listener {
public:
    PatternEditor() {
        addAndMakeVisible(euclideanViewer);
        addAndMakeVisible(pianoRollViewer);

        euclideanViewer.onHitMapToggled = [this](const std::vector<uint8_t>& map) {
            if (onEuclideanHitMapChanged) onEuclideanHitMapChanged(map);
        };

        auto setupSlider = [this](juce::Slider& sl, int min, int max, int val, const juce::String& name) {
            sl.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
            sl.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 50, 20);
            sl.setRange(min, max, 1);
            sl.setValue(val);
            sl.addListener(this);
            sl.setName(name);
            addAndMakeVisible(sl);
        };

        setupSlider(stepsSlider, 1, 64, 16, "Steps");
        setupSlider(pulsesSlider, 0, 64, 4, "Pulses");

        modeSelector.addItem("Piano Roll", 1);
        modeSelector.addItem("Euclidean", 2);
        modeSelector.setSelectedId(2);
        modeSelector.onChange = [this]() { updateMode(); };
        addAndMakeVisible(modeSelector);

        updateMode();
    }

    void resized() override {
        auto bounds = getLocalBounds();
        auto controls = bounds.removeFromLeft(120);
        
        modeSelector.setBounds(controls.removeFromTop(30).reduced(2));
        stepsSlider.setBounds(controls.removeFromTop(80).reduced(2));
        pulsesSlider.setBounds(controls.removeFromTop(80).reduced(2));

        euclideanViewer.setBounds(bounds);
        pianoRollViewer.setBounds(bounds);
    }

    void sliderValueChanged(juce::Slider* slider) override {
        juce::ignoreUnused(slider);
        
        if (modeSelector.getSelectedId() == 2) {
            int k = (int)pulsesSlider.getValue();
            int n = (int)stepsSlider.getValue();
            euclideanViewer.setParams(k, n);
            // Fire callback so MainComponent can push a command to the audio thread
            if (onEuclideanChanged) onEuclideanChanged(k, n); 
        }
    }

    void updateMode() {
        bool isEuc = modeSelector.getSelectedId() == 2;
        euclideanViewer.setVisible(isEuc);
        pianoRollViewer.setVisible(!isEuc);
        stepsSlider.setVisible(isEuc);
        pulsesSlider.setVisible(isEuc);
    }

    void setPlayheadPhase(float phase) {
        if (modeSelector.getSelectedId() == 2) {
            euclideanViewer.setPlayheadPhase(phase);
        }
    }

    // Load clip state into the editor without triggering audio callbacks
    void loadClipData (const ClipData& clip)
    {
        stepsSlider.setValue  (clip.euclideanSteps,  juce::dontSendNotification);
        pulsesSlider.setValue (clip.euclideanPulses, juce::dontSendNotification);
        euclideanViewer.setParams (clip.euclideanPulses, clip.euclideanSteps);

        if (!clip.hitMap.empty() && (int) clip.hitMap.size() == clip.euclideanSteps)
        {
            euclideanViewer.activeSteps = clip.hitMap;
            euclideanViewer.repaint();
        }

        modeSelector.setSelectedId (2, juce::dontSendNotification);
        updateMode();
    }

    std::function<void(int pulses, int steps)> onEuclideanChanged;
    std::function<void(const std::vector<uint8_t>&)> onEuclideanHitMapChanged;

private:
    EuclideanCircleViewer euclideanViewer;
    PianoRollViewer pianoRollViewer;
    juce::Slider stepsSlider;
    juce::Slider pulsesSlider;
    juce::ComboBox modeSelector;
};
