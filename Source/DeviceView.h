#pragma once
#include <JuceHeader.h>
#include "SimplerComponent.h"

#include <functional>

class DeviceView : public juce::Component, public juce::DragAndDropTarget, public juce::FileDragAndDropTarget {
public:
    std::function<void(const juce::File&)> onFileDropped;

    DeviceView() {
        setOpaque(true);
    }

    void paint(juce::Graphics& g) override {
        g.fillAll(juce::Colours::darkgrey.darker().darker());
        g.setColour(juce::Colours::black);
        g.drawRect(getLocalBounds(), 2);

        if (simpler == nullptr) {
            g.setColour(juce::Colours::white);
            g.drawText("Device View: Drop Audio File Here", getLocalBounds(), juce::Justification::centred);
        }
    }

    void resized() override {
        if (simpler != nullptr) {
            simpler->setBounds(getLocalBounds().reduced(2));
        }
    }

    bool loadAudioFile(const juce::File& file) {
        auto ext = file.getFileExtension().toLowerCase();
        if (ext == ".wav" || ext == ".aiff" || ext == ".aif" || ext == ".flac" || ext == ".mp3" || ext == ".ogg") {
            if (simpler == nullptr) {
                simpler = std::make_unique<SimplerComponent>();
                addAndMakeVisible(simpler.get());
            }
            simpler->loadFile(file);
            if (onFileDropped) onFileDropped(file);
            resized();
            repaint();
            return true;
        }
        return false;
    }

    void clear() {
        if (simpler != nullptr) {
            simpler.reset();
            resized();
            repaint();
        }
    }

    // Pure display update — called by MainComponent when the user switches track selection.
    // Lazily creates the SimplerComponent if needed. Does NOT fire onFileDropped.
    void showFile(const juce::File& file) {
        if (!file.existsAsFile()) {
            clear();
            return;
        }
        if (simpler == nullptr) {
            simpler = std::make_unique<SimplerComponent>();
            addAndMakeVisible(simpler.get());
            resized();
        }
        simpler->showFile(file);
        repaint();
    }

    // -- Internal JUCE Drag and Drop --
    bool isInterestedInDragSource(const juce::DragAndDropTarget::SourceDetails& details) override {
        return details.description.toString() == "FileBrowserDrag" || dynamic_cast<juce::FileTreeComponent*>(details.sourceComponent.get()) != nullptr;
    }

    void itemDropped(const juce::DragAndDropTarget::SourceDetails& details) override {
        if (auto* tree = dynamic_cast<juce::FileTreeComponent*>(details.sourceComponent.get())) {
            juce::File file = tree->getSelectedFile(0);
            if (file.existsAsFile()) {
                loadAudioFile(file);
            }
        }
    }

    // -- OS File Drag and Drop --
    bool isInterestedInFileDrag(const juce::StringArray& files) override {
        for (auto& f : files) {
            auto ext = juce::File(f).getFileExtension().toLowerCase();
            if (ext == ".wav" || ext == ".aiff" || ext == ".aif" || ext == ".flac" || ext == ".mp3" || ext == ".ogg")
                return true;
        }
        return false;
    }

    void filesDropped(const juce::StringArray& files, int, int) override {
        for (auto& f : files) {
            if (loadAudioFile(juce::File(f))) {
                break;
            }
        }
    }

private:
    std::unique_ptr<SimplerComponent> simpler;
};
