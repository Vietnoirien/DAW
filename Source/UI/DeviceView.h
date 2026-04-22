#pragma once
#include <JuceHeader.h>
#include <functional>

class DeviceChainContainer : public juce::Component {
public:
    juce::OwnedArray<juce::Component> editors;

    void addEditor(std::unique_ptr<juce::Component> editor) {
        if (editor != nullptr) {
            addAndMakeVisible(editor.get());
            editors.add(editor.release());
            updateLayout();
        }
    }

    void clear() {
        editors.clear();
        updateLayout();
    }

    void updateLayout() {
        int x = 4;
        int h = std::max(10, getHeight() - 8);
        for (auto* ed : editors) {
            ed->setBounds(x, 4, ed->getWidth(), h);
            x += ed->getWidth() + 8;
        }
        if (getWidth() != x) {
            setSize(x, getHeight());
        }
    }

    void resized() override {
        updateLayout();
    }

    void paint(juce::Graphics& g) override {
        if (editors.isEmpty()) {
            g.setColour(juce::Colours::white);
            g.drawText("Device View: Drop Audio File or Instrument Here", getLocalBounds(), juce::Justification::centred);
        }
    }
};

class DeviceView : public juce::Component, public juce::DragAndDropTarget, public juce::FileDragAndDropTarget {
public:
    std::function<void(const juce::File&)> onFileDropped;

    DeviceView() {
        setOpaque(true);
        viewport.setViewedComponent(&chainContainer, false);
        viewport.setScrollBarsShown(false, true);
        addAndMakeVisible(viewport);
    }

    void paint(juce::Graphics& g) override {
        g.fillAll(juce::Colours::darkgrey.darker().darker());
        g.setColour(juce::Colours::black);
        g.drawRect(getLocalBounds(), 2);
    }

    void resized() override {
        viewport.setBounds(getLocalBounds().reduced(2));
        chainContainer.setSize(chainContainer.getWidth(), viewport.getHeight());
    }

    void clear() {
        chainContainer.clear();
        repaint();
    }

    void addEditor(std::unique_ptr<juce::Component> newEditor) {
        chainContainer.addEditor(std::move(newEditor));
        repaint();
    }
    
    // Kept for backward compatibility with existing code where DrumRack needs pad selection logic
    juce::Component* getFirstEditor() const {
        if (chainContainer.editors.isEmpty()) return nullptr;
        return chainContainer.editors.getFirst();
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

    DeviceChainContainer chainContainer;
    juce::Viewport viewport;
};
