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
        int maxH = 0;
        for (auto* ed : editors) {
            // Use each editor's own preferred height (set via setSize in its ctor)
            int h = juce::jmax(10, ed->getHeight());
            ed->setBounds(x, 4, ed->getWidth(), h);
            x += ed->getWidth() + 8;
            maxH = juce::jmax(maxH, h);
        }
        // Resize container so the parent DeviceView can read the correct height
        int newW = juce::jmax(getWidth(), x);
        int newH = maxH > 0 ? maxH + 8 : getHeight();
        if (getWidth() != newW || getHeight() != newH)
            setSize(newW, newH);
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
    std::function<void(const juce::String&)> onParameterTouched;
    std::function<bool()> canAddAutomation;
    // Returns true if the selected clip already has an automation lane for this parameter
    std::function<bool(const juce::String&)> hasAutomationForParam;
    // Fired after any slider/knob/button inside the device chain is released.
    // Wire this to MainComponent::markDirty() to track unsaved parameter changes.
    std::function<void()> onParamChanged;
    // Fired when the user starts interacting with a control.
    std::function<void()> onParamDragStart;

    DeviceView() {
        setOpaque(true);
        viewport.setViewedComponent(&chainContainer, false);
        viewport.setScrollBarsShown(false, true);
        addAndMakeVisible(viewport);
        
        // Listen to mouse events from all child components (sliders)
        addMouseListener(this, true);
    }

    void mouseDown(const juce::MouseEvent& event) override {
        if (event.mods.isPopupMenu()) {
            if (canAddAutomation && !canAddAutomation()) return;

            if (auto* c = event.originalComponent) {
                juce::Component* target = c;
                juce::String paramId = target->getProperties()["parameterId"].toString();
                while (target && paramId.isEmpty()) {
                    target = target->getParentComponent();
                    if (target) paramId = target->getProperties()["parameterId"].toString();
                }

                if (paramId.isNotEmpty()) {
                    bool hasLane = hasAutomationForParam && hasAutomationForParam(paramId);
                    juce::String label = (hasLane ? "Edit Automation: " : "Add Automation: ") + paramId;
                    juce::PopupMenu m;
                    m.addItem(1, label);
                    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(c),
                        [this, paramId](int result) {
                            if (result == 1 && onParameterTouched) {
                                onParameterTouched(paramId);
                            }
                        });
                }
            }
        }
        else if (!event.mods.isPopupMenu()) {
            if (event.originalComponent != this && onParamDragStart) {
                onParamDragStart();
            }
        }
    }

    // Fired when the user releases the mouse after interacting with any child
    // control (slider, knob, button). One call per user gesture — not on every
    // intermediate drag tick — so markDirty() won't flood on fader drags.
    void mouseUp(const juce::MouseEvent& event) override
    {
        // Only count interactions with actual controls, not the DeviceView background.
        if (event.originalComponent != this && onParamChanged)
            onParamChanged();
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

    struct AutomationParamInfo { bool isAutomated; float minNorm; float maxNorm; };

    void updateAutomationIndicators(const std::unordered_map<juce::String, AutomationParamInfo>& params) {
        std::function<void(juce::Component*)> walk = [&](juce::Component* c) {
            auto paramId = c->getProperties()["parameterId"].toString();
            if (paramId.isNotEmpty()) {
                auto it = params.find(paramId);
                bool isAuto = (it != params.end()) && it->second.isAutomated;
                c->getProperties().set("isAutomated",       isAuto);
                c->getProperties().set("automationMinNorm", isAuto ? it->second.minNorm : 0.0f);
                c->getProperties().set("automationMaxNorm", isAuto ? it->second.maxNorm : 1.0f);
                c->repaint();
            }
            for (auto* child : c->getChildren()) walk(child);
        };
        walk(&chainContainer);
    }

    // Legacy overload — called with just a list of automated param IDs (no range info)
    void updateAutomationIndicators(const std::vector<juce::String>& automatedParams) {
        std::unordered_map<juce::String, AutomationParamInfo> m;
        for (const auto& id : automatedParams)
            m[id] = { true, 0.0f, 1.0f };
        updateAutomationIndicators(m);
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
