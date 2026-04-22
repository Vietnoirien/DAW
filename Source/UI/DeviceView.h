#pragma once
#include <JuceHeader.h>
#include <functional>

class DeviceView : public juce::Component, public juce::DragAndDropTarget, public juce::FileDragAndDropTarget {
public:
    std::function<void(const juce::File&)> onFileDropped;

    DeviceView() {
        setOpaque(true);
    }

    juce::Component* getCurrentEditor() const {
        return currentEditor.get();
    }

    void paint(juce::Graphics& g) override {
        g.fillAll(juce::Colours::darkgrey.darker().darker());
        g.setColour(juce::Colours::black);
        g.drawRect(getLocalBounds(), 2);

        if (currentEditor == nullptr) {
            g.setColour(juce::Colours::white);
            g.drawText("Device View: Drop Audio File or Instrument Here", getLocalBounds(), juce::Justification::centred);
        }
    }

    void resized() override {
        if (currentEditor != nullptr) {
            currentEditor->setBounds(getLocalBounds().reduced(2));
        }
    }

    void showEditor(std::unique_ptr<juce::Component> newEditor) {
        if (currentEditor != nullptr) {
            removeChildComponent(currentEditor.get());
        }
        
        currentEditor = std::move(newEditor);
        
        if (currentEditor != nullptr) {
            addAndMakeVisible(currentEditor.get());
            resized();
        }
        
        repaint();
    }

    void clear() {
        showEditor(nullptr);
    }

    // -- Internal JUCE Drag and Drop --
    bool isInterestedInDragSource(const juce::DragAndDropTarget::SourceDetails& details) override {
        return details.description.toString() == "FileBrowserDrag" || dynamic_cast<juce::FileTreeComponent*>(details.sourceComponent.get()) != nullptr;
    }

    void itemDropped(const juce::DragAndDropTarget::SourceDetails& details) override {
        if (auto* tree = dynamic_cast<juce::FileTreeComponent*>(details.sourceComponent.get())) {
            juce::File file = tree->getSelectedFile(0);
            if (file.existsAsFile() && isAudioFile(file)) {
                if (onFileDropped) onFileDropped(file);
            }
        }
    }

    // -- OS File Drag and Drop --
    bool isInterestedInFileDrag(const juce::StringArray& files) override {
        for (auto& f : files) {
            if (isAudioFile(juce::File(f))) return true;
        }
        return false;
    }

    void filesDropped(const juce::StringArray& files, int, int) override {
        for (auto& f : files) {
            juce::File file(f);
            if (isAudioFile(file)) {
                if (onFileDropped) onFileDropped(file);
                break;
            }
        }
    }

private:
    bool isAudioFile(const juce::File& file) const {
        auto ext = file.getFileExtension().toLowerCase();
        return (ext == ".wav" || ext == ".aiff" || ext == ".aif" || ext == ".flac" || ext == ".mp3" || ext == ".ogg");
    }

    std::unique_ptr<juce::Component> currentEditor;
};
