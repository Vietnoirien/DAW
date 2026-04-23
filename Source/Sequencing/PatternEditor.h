#pragma once
#include <JuceHeader.h>
#include <cmath>
#include "ClipData.h"
#include "AutomationEditor.h"

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
            g.setFont(juce::Font(juce::FontOptions(12.0f, isActiveStep ? juce::Font::bold : juce::Font::plain)));
            g.drawText(juce::String(i + 1), numP.x - 10.0f, numP.y - 10.0f, 20.0f, 20.0f, juce::Justification::centred);
        }

        // Draw rotate arrows
        juce::Rectangle<float> leftBtn(bounds.getX() - 40.0f, bounds.getCentreY() - 15.0f, 30.0f, 30.0f);
        juce::Rectangle<float> rightBtn(bounds.getRight() + 10.0f, bounds.getCentreY() - 15.0f, 30.0f, 30.0f);
        
        g.setColour(juce::Colours::white.withAlpha(0.6f));
        g.setFont(juce::Font(juce::FontOptions(24.0f, juce::Font::bold)));
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

#include "PianoRollEditor.h"
class PatternEditor : public juce::Component, public juce::Slider::Listener {
public:
    PatternEditor() {
        addAndMakeVisible(euclideanViewer);
        addAndMakeVisible(pianoRollViewer);
        addAndMakeVisible(automationViewer);

        euclideanViewer.onHitMapToggled = [this](const std::vector<uint8_t>& map) {
            if (onEuclideanHitMapChanged) onEuclideanHitMapChanged(map);
        };

        pianoRollViewer.onNotesChanged = [this](const std::vector<MidiNote>& notes) {
            if (onMidiNotesChanged) onMidiNotesChanged(notes);
        };

        automationViewer.onAutomationChanged = [this]() {
            if (onAutomationLaneChanged) onAutomationLaneChanged();
        };

        auditionToggle.setButtonText("Audition");
        auditionToggle.setToggleState(false, juce::dontSendNotification);
        addAndMakeVisible(auditionToggle);

        pianoRollViewer.onAuditionNoteOn = [this](int note, int vel) {
            if (auditionToggle.getToggleState() && onAuditionNoteOn)
                onAuditionNoteOn(note, vel);
        };
        pianoRollViewer.onAuditionNoteOff = [this](int note) {
            if (auditionToggle.getToggleState() && onAuditionNoteOff)
                onAuditionNoteOff(note);
        };

        auto setupSlider = [this](juce::Slider& sl, int min, int max, int val, const juce::String& name) {
            sl.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
            sl.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 50, 20);
            sl.setRange(min, max, 1);
            sl.setValue(val);
            sl.setDoubleClickReturnValue(true, val);
            sl.addListener(this);
            sl.setName(name);
            addAndMakeVisible(sl);
        };

        setupSlider(stepsSlider, 1, 64, 16, "Steps");
        setupSlider(pulsesSlider, 0, 64, 4, "Pulses");

        modeSelector.addItem("Piano Roll", 1);
        modeSelector.addItem("Euclidean", 2);
        modeSelector.addItem("Automation", 3);
        modeSelector.setSelectedId(2);
        modeSelector.onChange = [this]() {
            updateMode();
            if (onModeChanged) {
                juce::String modeStr = "euclidean";
                if (modeSelector.getSelectedId() == 1) modeStr = "pianoroll";
                else if (modeSelector.getSelectedId() == 3) modeStr = "automation";
                onModeChanged(modeStr);
            }
        };
        addAndMakeVisible(modeSelector);

        updateMode();
    }

    void resized() override {
        auto bounds = getLocalBounds();
        auto controls = bounds.removeFromLeft(120);
        
        modeSelector.setBounds(controls.removeFromTop(30).reduced(2));
        auditionToggle.setBounds(controls.removeFromTop(30).reduced(2));
        stepsSlider.setBounds(controls.removeFromTop(80).reduced(2));
        pulsesSlider.setBounds(controls.removeFromTop(80).reduced(2));

        euclideanViewer.setBounds(bounds);
        pianoRollViewer.setBounds(bounds);
        automationViewer.setBounds(bounds);
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
        int modeId = modeSelector.getSelectedId();
        bool isEuc = modeId == 2;
        bool isAuto = modeId == 3;
        
        euclideanViewer.setVisible(isEuc);
        pianoRollViewer.setVisible(modeId == 1);
        automationViewer.setVisible(isAuto);
        
        stepsSlider.setVisible(isEuc);
        pulsesSlider.setVisible(isEuc);
        auditionToggle.setVisible(modeId == 1);
    }

    void setPlayheadPhase(float phase) {
        if (modeSelector.getSelectedId() == 2) {
            euclideanViewer.setPlayheadPhase(phase);
        } else if (modeSelector.getSelectedId() == 3) {
            automationViewer.setPlayheadPhase(phase);
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

        pianoRollViewer.setNotes(clip.midiNotes);

        int modeId = 2; // Euclidean
        if (clip.patternMode == "pianoroll") modeId = 1;
        else if (clip.patternMode == "automation") modeId = 3;
        else if (clip.patternMode == "drumrack") modeId = 1; // DrumRack uses piano roll mostly

        modeSelector.setSelectedId (modeId, juce::dontSendNotification);
        updateMode();
    }

    void loadDrumPadData(int steps, int pulses, const std::vector<uint8_t>& hitMap) {
        stepsSlider.setValue(steps, juce::dontSendNotification);
        pulsesSlider.setValue(pulses, juce::dontSendNotification);
        euclideanViewer.setParams(pulses, steps);
        if (!hitMap.empty() && (int)hitMap.size() == steps) {
            euclideanViewer.activeSteps = hitMap;
        } else if (hitMap.empty()) {
            euclideanViewer.setParams(pulses, steps); // resets hitMap internally
        }
        euclideanViewer.repaint();
        modeSelector.setSelectedId(2, juce::dontSendNotification);
        updateMode();
    }

    std::function<void(int pulses, int steps)> onEuclideanChanged;
    std::function<void(const std::vector<uint8_t>&)> onEuclideanHitMapChanged;
    std::function<void(const std::vector<MidiNote>&)> onMidiNotesChanged;
    std::function<void(const juce::String&)> onModeChanged;
    std::function<void(int, int)> onAuditionNoteOn;
    std::function<void(int)> onAuditionNoteOff;
    std::function<void()> onAutomationLaneChanged;

    void setActiveAutomationLane(AutomationLane* lane, const juce::String& paramId, double patternLengthBars) {
        automationViewer.setAutomationLane(lane, patternLengthBars > 0.0 ? patternLengthBars * 4.0 : 4.0);
        automationViewer.setParameterId(paramId);
        
        // If the user touched a parameter, automatically switch to the automation view
        // to make it easy to edit what they just touched
        if (lane && !paramId.isEmpty()) {
            modeSelector.setSelectedId(3, juce::sendNotificationSync);
        }
    }

private:
    EuclideanCircleViewer euclideanViewer;
    PianoRollViewer pianoRollViewer;
    AutomationEditorViewer automationViewer;
    juce::Slider stepsSlider;
    juce::Slider pulsesSlider;
    juce::ComboBox modeSelector;
    juce::ToggleButton auditionToggle;
};
