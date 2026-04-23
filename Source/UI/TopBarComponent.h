#pragma once
#include <JuceHeader.h>
#include "GlobalTransport.h"

class TopBarComponent : public juce::Component {
public:
    TopBarComponent(GlobalTransport& t, juce::AudioDeviceManager& dm)
        : transport(t), deviceManager(dm)
    {
        addAndMakeVisible(playBtn);
        addAndMakeVisible(stopBtn);
        addAndMakeVisible(bpmSlider);
        addAndMakeVisible(loadPluginBtn);
        addAndMakeVisible(settingsBtn);
        addAndMakeVisible(projectBtn);
        addAndMakeVisible(sessionViewBtn);
        addAndMakeVisible(arrangeViewBtn);

        sessionViewBtn.setClickingTogglesState(true);
        arrangeViewBtn.setClickingTogglesState(true);
        sessionViewBtn.setRadioGroupId(1);
        arrangeViewBtn.setRadioGroupId(1);
        sessionViewBtn.setToggleState(true, juce::dontSendNotification);

        sessionViewBtn.onClick = [this] {
            if (sessionViewBtn.getToggleState() && onSwitchToSession)
                onSwitchToSession();
        };
        arrangeViewBtn.onClick = [this] {
            if (arrangeViewBtn.getToggleState() && onSwitchToArrangement)
                onSwitchToArrangement();
        };

        bpmSlider.setSliderStyle(juce::Slider::LinearBar);
        bpmSlider.setRange(20.0, 300.0, 1.0);
        bpmSlider.setValue(transport.getBpm());
        bpmSlider.setDoubleClickReturnValue(true, 120.0);
        bpmSlider.onValueChange = [this] { transport.setBpm(bpmSlider.getValue()); };

        settingsBtn.onClick = [this] { openAudioSettings(); };

        projectBtn.onClick = [this] {
            juce::PopupMenu menu;
            menu.addItem(1, "New Project");
            menu.addItem(2, "Open Project...");
            menu.addSeparator();
            menu.addItem(3, "Save Project");
            menu.addItem(4, "Save Project As...");
            menu.addSeparator();
            menu.addItem(5, "Export Audio...");

            menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&projectBtn),
                [this](int result) {
                    if (result == 1 && onNewProject) onNewProject();
                    if (result == 2 && onOpenProject) onOpenProject();
                    if (result == 3 && onSaveProject) onSaveProject();
                    if (result == 4 && onSaveProjectAs) onSaveProjectAs();
                    if (result == 5 && onExportAudio) onExportAudio();
                });
        };

        setOpaque(true);
    }

    // Callbacks for project management
    std::function<void()> onNewProject;
    std::function<void()> onOpenProject;
    std::function<void()> onSaveProject;
    std::function<void()> onSaveProjectAs;
    std::function<void()> onExportAudio;

    // View callbacks
    std::function<void()> onSwitchToSession;
    std::function<void()> onSwitchToArrangement;

    // Called by MainComponent after the settings dialog closes.
    // Assign a lambda here to persist the device state.
    std::function<void()> onSettingsClosed;

    void paint(juce::Graphics& g) override {
        g.fillAll(juce::Colours::darkgrey.darker());
        g.setColour(juce::Colours::black);
        g.drawLine(0.0f, (float)getHeight(), (float)getWidth(), (float)getHeight(), 2.0f);

        g.setColour(juce::Colours::white);
        juce::String timeText = juce::String(transport.getPlayheadPosition()) + " samples";
        g.drawText("Clock: " + timeText, getLocalBounds().withRight(getWidth() - 20), juce::Justification::centredRight);
    }

    void resized() override {
        juce::FlexBox fb;
        fb.alignContent = juce::FlexBox::AlignContent::center;
        fb.alignItems   = juce::FlexBox::AlignItems::center;
        fb.justifyContent = juce::FlexBox::JustifyContent::flexStart;

        auto margin = juce::FlexItem::Margin(5.0f);
        fb.items.add(juce::FlexItem(projectBtn).withWidth(90).withHeight(30).withMargin(margin));
        fb.items.add(juce::FlexItem(playBtn).withWidth(80).withHeight(30).withMargin(margin));
        fb.items.add(juce::FlexItem(stopBtn).withWidth(80).withHeight(30).withMargin(margin));
        fb.items.add(juce::FlexItem(bpmSlider).withWidth(100).withHeight(30).withMargin(margin));
        
        // Add some space before view buttons
        juce::FlexBox viewFb;
        viewFb.alignContent = juce::FlexBox::AlignContent::center;
        viewFb.alignItems   = juce::FlexBox::AlignItems::center;
        viewFb.items.add(juce::FlexItem(sessionViewBtn).withWidth(70).withHeight(30));
        viewFb.items.add(juce::FlexItem(arrangeViewBtn).withWidth(70).withHeight(30));
        
        fb.items.add(juce::FlexItem(viewFb).withWidth(140).withHeight(30).withMargin(margin));
        
        fb.items.add(juce::FlexItem(loadPluginBtn).withWidth(120).withHeight(30).withMargin(margin));
        fb.items.add(juce::FlexItem(settingsBtn).withWidth(110).withHeight(30).withMargin(margin));

        fb.performLayout(getLocalBounds());
    }

    juce::TextButton playBtn      {"Play"};
    juce::TextButton stopBtn      {"Stop"};
    juce::TextButton loadPluginBtn{"Load Plugin..."};
    juce::TextButton settingsBtn  {"\u2699 Settings"};
    juce::TextButton projectBtn   {"Project \u25BC"};
    juce::TextButton sessionViewBtn{"SESSION"};
    juce::TextButton arrangeViewBtn{"ARRANGE"};
    juce::Slider     bpmSlider;

private:
    void openAudioSettings()
    {
        // AudioDeviceSelectorComponent is JUCE's built-in panel for picking:
        //  - Output device  (show Sapphire 6 USB here)
        //  - Sample rate    (44100 / 48000 / 96000 Hz)
        //  - Buffer size    (crucial for latency — set too low and USB will stutter)
        //  - Input channels, MIDI devices
        auto* selector = new juce::AudioDeviceSelectorComponent(
            deviceManager,
            0,     // min audio inputs
            6,     // max audio inputs  (Sapphire 6 has 6 ins)
            2,     // min audio outputs
            6,     // max audio outputs (Sapphire 6 has 6 outs)
            true,  // show MIDI inputs
            false, // show MIDI outputs
            true,  // show as stereo pairs
            false  // hide advanced options button
        );
        selector->setSize(500, 440);

        juce::DialogWindow::LaunchOptions opts;
        opts.content.setOwned(selector);
        opts.dialogTitle                  = "Audio & MIDI Settings";
        opts.componentToCentreAround      = this;
        opts.dialogBackgroundColour       = juce::Colour(0xff2a2a2a);
        opts.escapeKeyTriggersCloseButton = true;
        opts.useNativeTitleBar            = true;
        opts.resizable                    = false;

        // Capture the callback; save settings when dialog is dismissed.
        auto cb = onSettingsClosed;
        opts.dialogTitle; // (suppress unused warning — opts already set)
        if (auto* dw = opts.launchAsync())
        {
            // The DialogWindow is reference-counted; we attach a ComponentListener
            // that fires once the window is about to be deleted (i.e. closed).
            struct CloseListener : public juce::ComponentListener
            {
                std::function<void()> callback;
                explicit CloseListener(std::function<void()> c) : callback(std::move(c)) {}
                void componentBeingDeleted(juce::Component&) override
                {
                    if (callback) callback();
                    delete this; // self-owned
                }
            };
            if (cb)
                dw->addComponentListener(new CloseListener(std::move(cb)));
        }
    }

    GlobalTransport&          transport;
    juce::AudioDeviceManager& deviceManager;
};
