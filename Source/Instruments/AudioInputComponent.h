#pragma once
#include <JuceHeader.h>
#include "AudioInputProcessor.h"
#include "../Core/MainComponent.h"

class AudioInputComponent : public juce::Component, public juce::Timer
{
public:
    explicit AudioInputComponent(AudioInputProcessor& p) : processor(p)
    {
        // Background styling
        setOpaque(true);

        // Input Channel Label & Combo
        channelLabel.setText("Audio From", juce::dontSendNotification);
        channelLabel.setFont(12.0f);
        channelLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
        channelLabel.setJustificationType(juce::Justification::centred);
        addAndMakeVisible(channelLabel);

        channelCombo.addItem("Input 1", 1);
        channelCombo.addItem("Input 2", 2);
        channelCombo.addItem("Input 3", 3);
        channelCombo.addItem("Input 4", 4);
        channelCombo.addItem("Stereo 1/2", 5);
        channelCombo.setJustificationType(juce::Justification::centred);
        
        if (auto* track = processor.getHostTrack()) {
            int ch = track->recordInputChannel.load();
            channelCombo.setSelectedId(ch + 1, juce::dontSendNotification);
        } else {
            channelCombo.setSelectedId(1, juce::dontSendNotification);
        }

        channelCombo.onChange = [this] {
            if (auto* track = processor.getHostTrack()) {
                track->recordInputChannel.store(channelCombo.getSelectedId() - 1);
            }
        };
        addAndMakeVisible(channelCombo);

        // Monitoring Label & Buttons
        monitorLabel.setText("Monitor", juce::dontSendNotification);
        monitorLabel.setFont(12.0f);
        monitorLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
        monitorLabel.setJustificationType(juce::Justification::centred);
        addAndMakeVisible(monitorLabel);

        monitorAutoBtn.setButtonText("Auto");
        monitorInBtn.setButtonText("In");
        monitorOffBtn.setButtonText("Off");

        // Styling for monitor buttons
        auto styleMonitorBtn = [](juce::TextButton& btn) {
            btn.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff333333));
            btn.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xfff6b26b));
            btn.setColour(juce::TextButton::textColourOffId, juce::Colours::grey);
            btn.setColour(juce::TextButton::textColourOnId, juce::Colours::black);
            btn.setRadioGroupId(101);
            btn.setClickingTogglesState(true);
        };

        styleMonitorBtn(monitorAutoBtn);
        styleMonitorBtn(monitorInBtn);
        styleMonitorBtn(monitorOffBtn);

        addAndMakeVisible(monitorAutoBtn);
        addAndMakeVisible(monitorInBtn);
        addAndMakeVisible(monitorOffBtn);

        bool isMonEnabled = false;
        if (auto* track = processor.getHostTrack()) {
            isMonEnabled = track->monitorEnabled.load();
        }
        
        if (isMonEnabled) monitorInBtn.setToggleState(true, juce::dontSendNotification);
        else monitorOffBtn.setToggleState(true, juce::dontSendNotification);

        auto onMonitorChange = [this] {
            if (auto* track = processor.getHostTrack()) {
                track->monitorEnabled.store(monitorInBtn.getToggleState());
                // Auto could be linked to track->isArmedForRecord, handled externally or dynamically.
            }
        };

        monitorAutoBtn.onClick = onMonitorChange;
        monitorInBtn.onClick = onMonitorChange;
        monitorOffBtn.onClick = onMonitorChange;

        // Gain Slider
        gainSlider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        gainSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 50, 16);
        gainSlider.setRange(0.0, 2.0, 0.01);
        if (auto* track = processor.getHostTrack()) {
            gainSlider.setValue(track->monitorGain.load(), juce::dontSendNotification);
        } else {
            gainSlider.setValue(1.0, juce::dontSendNotification);
        }
        gainSlider.onValueChange = [this] {
            if (auto* track = processor.getHostTrack()) {
                track->monitorGain.store((float)gainSlider.getValue());
            }
        };
        addAndMakeVisible(gainSlider);

        gainLabel.setText("Gain", juce::dontSendNotification);
        gainLabel.setFont(12.0f);
        gainLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
        gainLabel.setJustificationType(juce::Justification::centred);
        addAndMakeVisible(gainLabel);

        setSize(250, 180);
        startTimerHz(15);
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour(0xff222222));
        g.setColour(juce::Colour(0xff333333));
        g.drawRect(getLocalBounds(), 1);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(10);
        
        auto topArea = area.removeFromTop(40);
        channelLabel.setBounds(topArea.removeFromLeft(80));
        channelCombo.setBounds(topArea.reduced(0, 8));

        area.removeFromTop(10);

        auto midArea = area.removeFromTop(30);
        monitorLabel.setBounds(midArea.removeFromLeft(60));
        int btnW = midArea.getWidth() / 3;
        monitorAutoBtn.setBounds(midArea.removeFromLeft(btnW).reduced(2));
        monitorInBtn.setBounds(midArea.removeFromLeft(btnW).reduced(2));
        monitorOffBtn.setBounds(midArea.reduced(2));

        area.removeFromTop(10);

        auto botArea = area;
        gainSlider.setBounds(botArea.withSizeKeepingCentre(60, 70));
        gainLabel.setBounds(gainSlider.getBounds().translated(0, -35).withHeight(20));
    }

    void timerCallback() override
    {
        // Keep UI in sync with backend changes
        if (auto* track = processor.getHostTrack()) {
            int ch = track->recordInputChannel.load();
            if (channelCombo.getSelectedId() != ch + 1) {
                channelCombo.setSelectedId(ch + 1, juce::dontSendNotification);
            }
            
            bool enabled = track->monitorEnabled.load();
            if (enabled && !monitorInBtn.getToggleState()) {
                monitorInBtn.setToggleState(true, juce::dontSendNotification);
            } else if (!enabled && monitorInBtn.getToggleState()) {
                monitorOffBtn.setToggleState(true, juce::dontSendNotification);
            }
            
            float gain = track->monitorGain.load();
            if (std::abs(gainSlider.getValue() - gain) > 0.01) {
                gainSlider.setValue(gain, juce::dontSendNotification);
            }
        }
    }

private:
    AudioInputProcessor& processor;

    juce::Label channelLabel;
    juce::ComboBox channelCombo;

    juce::Label monitorLabel;
    juce::TextButton monitorAutoBtn;
    juce::TextButton monitorInBtn;
    juce::TextButton monitorOffBtn;

    juce::Slider gainSlider;
    juce::Label gainLabel;
};
