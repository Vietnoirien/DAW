#pragma once
#include <JuceHeader.h>
#include "DrumRackProcessor.h"
#include "../Sequencing/PatternEditor.h"

class DrumRackComponent : public juce::Component, public juce::FileDragAndDropTarget, public juce::DragAndDropTarget, public juce::Timer {
public:
    DrumRackComponent(DrumRackProcessor* p) : processor(p) {
        // Pad Buttons
        for (int i = 0; i < 16; ++i) {
            auto btn = std::make_unique<juce::TextButton>(juce::String("Pad ") + juce::String(i + 1));
            btn->onClick = [this, i]() { selectPad(i); };
            addAndMakeVisible(*btn);
            padButtons.push_back(std::move(btn));
        }

        // Knobs
        auto setupKnob = [this](juce::Slider& sl, const juce::String& name, double min, double max, double val) {
            sl.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
            sl.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 50, 20);
            sl.setRange(min, max);
            sl.setValue(val);
            sl.onValueChange = [this, &sl, name]() {
                if (selectedPad >= 0 && selectedPad < 16) {
                    if (name == "Gain") processor->settings[selectedPad].gain.store(sl.getValue());
                    if (name == "Pan") processor->settings[selectedPad].pan.store(sl.getValue());
                    if (name == "Pitch") processor->settings[selectedPad].pitchOffset.store(sl.getValue());
                    if (name == "Decay") processor->settings[selectedPad].decay.store(sl.getValue());
                }
            };
            addAndMakeVisible(sl);
        };

        setupKnob(gainKnob, "Gain", 0.0, 2.0, 1.0);
        setupKnob(panKnob, "Pan", -1.0, 1.0, 0.0);
        setupKnob(pitchKnob, "Pitch", -24.0, 24.0, 0.0);
        setupKnob(decayKnob, "Decay", 0.05, 5.0, 0.5);

        // Thumbnail
        formatManager.registerBasicFormats();
        thumbnail = std::make_unique<juce::AudioThumbnail>(512, formatManager, thumbnailCache);

        selectPad(0);
        startTimerHz(30);
    }

    void resized() override {
        auto bounds = getLocalBounds();

        // 4x4 Pad Grid
        auto padArea = bounds.removeFromLeft(bounds.getWidth() / 3);
        int padW = padArea.getWidth() / 4;
        int padH = padArea.getHeight() / 4;
        for (int i = 0; i < 16; ++i) {
            int row = 3 - (i / 4); // Bottom up like MPC
            int col = i % 4;
            padButtons[i]->setBounds(padArea.getX() + col * padW, padArea.getY() + row * padH, padW, padH);
            padButtons[i]->setColour(juce::TextButton::buttonColourId, (i == selectedPad) ? juce::Colours::orange : juce::Colours::darkgrey);
        }

        // Params Area
        auto topArea = bounds.removeFromTop(bounds.getHeight() / 2);
        auto knobArea = topArea.removeFromRight(150);
        int kw = knobArea.getWidth() / 2;
        int kh = knobArea.getHeight() / 2;
        gainKnob.setBounds(knobArea.getX(), knobArea.getY(), kw, kh);
        panKnob.setBounds(knobArea.getX() + kw, knobArea.getY(), kw, kh);
        pitchKnob.setBounds(knobArea.getX(), knobArea.getY() + kh, kw, kh);
        decayKnob.setBounds(knobArea.getX() + kw, knobArea.getY() + kh, kw, kh);

        // Waveform Area
        waveformBounds = topArea.reduced(5);
    }

    void paint(juce::Graphics& g) override {
        g.fillAll(juce::Colour(0xff2d2d2d));

        // Draw waveform
        g.setColour(juce::Colours::black);
        g.fillRect(waveformBounds);

        g.setColour(juce::Colours::lightblue);
        if (thumbnail->getTotalLength() > 0.0) {
            thumbnail->drawChannels(g, waveformBounds, 0.0, thumbnail->getTotalLength(), 1.0f);
        } else {
            g.drawText("No Sample", waveformBounds, juce::Justification::centred);
        }
    }

    void timerCallback() override {
        if (selectedPad >= 0 && selectedPad < 16) {
            auto file = processor->settings[selectedPad].loadedFile;
            if (file != lastFile) {
                lastFile = file;
                if (file.existsAsFile()) {
                    thumbnail->setSource(new juce::FileInputSource(file));
                } else {
                    thumbnail->clear();
                }
                repaint();
            }
        }
    }

    void selectPad(int idx) {
        selectedPad = idx;
        
        // Update Knobs
        gainKnob.setValue(processor->settings[idx].gain.load(), juce::dontSendNotification);
        panKnob.setValue(processor->settings[idx].pan.load(), juce::dontSendNotification);
        pitchKnob.setValue(processor->settings[idx].pitchOffset.load(), juce::dontSendNotification);
        decayKnob.setValue(processor->settings[idx].decay.load(), juce::dontSendNotification);

        // Notify to update euclidean view
        if (onPadSelected) {
            onPadSelected(idx);
        }

        resized();
        repaint();
    }

    // Drag and Drop support
    bool isInterestedInFileDrag(const juce::StringArray& files) override {
        for (auto f : files) {
            juce::File file(f);
            auto ext = file.getFileExtension().toLowerCase();
            if (ext == ".wav" || ext == ".aiff" || ext == ".aif" || ext == ".flac" || ext == ".mp3" || ext == ".ogg") 
                return true;
        }
        return false;
    }

    void filesDropped(const juce::StringArray& files, int x, int y) override {
        if (files.isEmpty()) return;
        juce::File file(files[0]);
        if (!file.existsAsFile()) return;

        // Find which pad was dropped on
        for (int i = 0; i < 16; ++i) {
            if (padButtons[i]->getBounds().contains(x, y)) {
                if (onSampleDropped) onSampleDropped(i, file);
                selectPad(i);
                return;
            }
        }

        // If dropped outside pads but in the component, apply to selected pad
        if (selectedPad >= 0) {
            if (onSampleDropped) onSampleDropped(selectedPad, file);
        }
    }

    // Internal JUCE Drag and Drop support
    bool isInterestedInDragSource(const juce::DragAndDropTarget::SourceDetails& details) override {
        return details.description.toString() == "FileBrowserDrag" || dynamic_cast<juce::FileTreeComponent*>(details.sourceComponent.get()) != nullptr;
    }

    void itemDropped(const juce::DragAndDropTarget::SourceDetails& details) override {
        if (auto* tree = dynamic_cast<juce::FileTreeComponent*>(details.sourceComponent.get())) {
            juce::File file = tree->getSelectedFile(0);
            auto ext = file.getFileExtension().toLowerCase();
            if (file.existsAsFile() && (ext == ".wav" || ext == ".aiff" || ext == ".aif" || ext == ".flac" || ext == ".mp3" || ext == ".ogg")) {
                juce::StringArray arr;
                arr.add(file.getFullPathName());
                filesDropped(arr, details.localPosition.x, details.localPosition.y);
            }
        }
    }

    // Callbacks to MainComponent
    std::function<void(int padIndex)> onPadSelected;
    std::function<void(int padIndex, juce::File)> onSampleDropped;

private:
    DrumRackProcessor* processor;
    int selectedPad = 0;

    std::vector<std::unique_ptr<juce::TextButton>> padButtons;
    
    juce::Slider gainKnob;
    juce::Slider panKnob;
    juce::Slider pitchKnob;
    juce::Slider decayKnob;

    juce::AudioFormatManager formatManager;
    juce::AudioThumbnailCache thumbnailCache {5};
    std::unique_ptr<juce::AudioThumbnail> thumbnail;
    juce::Rectangle<int> waveformBounds;
    juce::File lastFile;
};
