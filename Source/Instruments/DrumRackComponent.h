#pragma once
#include <JuceHeader.h>
#include "DrumRackProcessor.h"
#include "../Sequencing/PatternEditor.h"
#include "../UI/LiBeLookAndFeel.h"

// ─── DrumRackComponent ────────────────────────────────────────────────────────
// Layout (320px tall):
//  ┌────────────────────────────────────────────────────────────────────────────┐
//  │  DRUM RACK  |  16 PADS                                         [title]    │
//  ├──────────────────────┬─────────────────────────────────────────────────────┤
//  │  [4x4 pad grid]      │  [waveform display]                                │
//  │                      │  ─────────────────────────────────────────────      │
//  │                      │  PAD CONTROLS                                       │
//  │                      │  [Gain] [Pan] [Pitch] [Decay]                      │
//  └──────────────────────┴─────────────────────────────────────────────────────┘
class DrumRackComponent : public juce::Component,
                          public juce::FileDragAndDropTarget,
                          public juce::DragAndDropTarget,
                          public juce::Timer
{
public:
    DrumRackComponent(DrumRackProcessor* p) : processor(p), laf(juce::Colour(0xffFF6600)) {
        setLookAndFeel(&laf);

        // 4x4 pad buttons
        for (int i = 0; i < 16; ++i) {
            auto btn = std::make_unique<juce::TextButton>(juce::String(i + 1));
            btn->onClick = [this, i] { selectPad(i); };
            addAndMakeVisible(*btn);
            padButtons.push_back(std::move(btn));
        }

        // Per-pad knobs
        auto sk = [&](LiBeKnob& k, const char* lbl, double v, double mn, double mx, double dv) {
            k.setup(lbl, v, mn, mx, dv, "", juce::Colour(0xffFF6600));
            addAndMakeVisible(k);
        };
        sk(kGain,  "Gain",   1.0, 0.0,   2.0,   1.0);
        sk(kPan,   "Pan",    0.0, -1.0,  1.0,   0.0);
        sk(kPitch, "Pitch",  0.0, -24.0, 24.0,  0.0);
        sk(kDecay, "Decay",  0.5, 0.05,  5.0,   0.5);

        kGain.slider.onValueChange  = [this] { if (selectedPad>=0) processor->settings[selectedPad].gain.store        ((float)kGain.slider.getValue()); };
        kPan.slider.onValueChange   = [this] { if (selectedPad>=0) processor->settings[selectedPad].pan.store         ((float)kPan.slider.getValue()); };
        kPitch.slider.onValueChange = [this] { if (selectedPad>=0) processor->settings[selectedPad].pitchOffset.store ((float)kPitch.slider.getValue()); };
        kDecay.slider.onValueChange = [this] { if (selectedPad>=0) processor->settings[selectedPad].decay.store       ((float)kDecay.slider.getValue()); };

        formatManager.registerBasicFormats();
        thumbnail = std::make_unique<juce::AudioThumbnail>(512, formatManager, thumbnailCache);

        selectPad(0);
        setSize(680, 320);
        startTimerHz(30);
    }

    ~DrumRackComponent() override { setLookAndFeel(nullptr); }

    void resized() override {
        auto b = getLocalBounds().withTrimmedTop(kTH);
        auto padArea = b.removeFromLeft(kPadAreaW).reduced(4);

        // 4x4 grid — row 0 = bottom (MPC style)
        int pw = padArea.getWidth() / 4, ph = padArea.getHeight() / 4;
        for (int i = 0; i < 16; ++i) {
            int col = i % 4, row = 3 - (i / 4);
            padButtons[i]->setBounds(padArea.getX() + col*pw, padArea.getY() + row*ph, pw, ph);
        }
        updatePadColors();

        // Right panel: waveform + knobs
        auto right = b.reduced(6, 4);
        waveformBounds = right.removeFromTop(kWaveH);
        right.removeFromTop(14); // "PAD CONTROLS" label space
        int x = right.getX(), y = right.getY();
        for (auto* k : std::initializer_list<LiBeKnob*>{ &kGain, &kPan, &kPitch, &kDecay }) {
            k->setBounds(x, y, LiBeKnob::kW, LiBeKnob::kH); x += LiBeKnob::kW + 6;
        }
    }

    void paint(juce::Graphics& g) override {
        g.fillAll(juce::Colour(0xff07070E));

        // Title bar
        g.setColour(juce::Colour(0xff120A04)); g.fillRect(0, 0, getWidth(), kTH);
        g.setColour(juce::Colour(0xffFF6600)); g.setFont(juce::FontOptions(13.0f, juce::Font::bold));
        g.drawText("DRUM RACK  |  16 PADS", 12, 0, getWidth()-12, kTH, juce::Justification::centredLeft);
        g.setColour(juce::Colour(0xff202030));
        g.drawHorizontalLine(kTH, 0.0f, (float)getWidth());
        g.drawVerticalLine(kPadAreaW, (float)kTH, (float)getHeight());

        // Waveform
        g.setColour(juce::Colour(0xff080808)); g.fillRect(waveformBounds);
        g.setColour(juce::Colour(0xff1A1008)); g.drawRect(waveformBounds, 1);
        if (thumbnail && thumbnail->getTotalLength() > 0.0) {
            g.setColour(juce::Colour(0xffFF8833));
            thumbnail->drawChannels(g, waveformBounds, 0.0, thumbnail->getTotalLength(), 1.0f);
        } else {
            g.setColour(juce::Colour(0xff555566));
            g.setFont(juce::FontOptions(10.0f));
            g.drawText("Drop Sample Here", waveformBounds, juce::Justification::centred);
        }

        // Section label
        g.setColour(juce::Colour(0xffFF6600).withAlpha(0.65f));
        g.setFont(juce::FontOptions(9.0f, juce::Font::bold));
        g.drawText("PAD CONTROLS", kPadAreaW + 6, waveformBounds.getBottom() + 2, 160, 12,
                   juce::Justification::centredLeft);
    }

    void timerCallback() override {
        if (selectedPad >= 0 && selectedPad < 16) {
            auto file = processor->settings[selectedPad].loadedFile;
            if (file != lastFile) {
                lastFile = file;
                if (file.existsAsFile())
                    thumbnail->setSource(new juce::FileInputSource(file));
                else
                    thumbnail->clear();
                repaint();
            }
        }
    }

    void selectPad(int idx) {
        selectedPad = idx;
        kGain.slider.setValue (processor->settings[idx].gain.load(),        juce::dontSendNotification);
        kPan.slider.setValue  (processor->settings[idx].pan.load(),         juce::dontSendNotification);
        kPitch.slider.setValue(processor->settings[idx].pitchOffset.load(), juce::dontSendNotification);
        kDecay.slider.setValue(processor->settings[idx].decay.load(),       juce::dontSendNotification);

        juce::String ps = "DrumRack/Pad " + juce::String(idx+1) + "/";
        kGain.slider.getProperties().set ("parameterId", ps + "Gain");
        kPan.slider.getProperties().set  ("parameterId", ps + "Pan");
        kPitch.slider.getProperties().set("parameterId", ps + "Pitch");
        kDecay.slider.getProperties().set("parameterId", ps + "Decay");

        updatePadColors();
        if (onPadSelected) onPadSelected(idx);
        repaint();
    }

    // File drag-and-drop
    bool isInterestedInFileDrag(const juce::StringArray& files) override {
        for (auto& f : files) { auto e = juce::File(f).getFileExtension().toLowerCase();
            if (e==".wav"||e==".aiff"||e==".aif"||e==".flac"||e==".mp3"||e==".ogg") return true; }
        return false;
    }
    void filesDropped(const juce::StringArray& files, int x, int y) override {
        if (files.isEmpty()) return;
        juce::File file(files[0]);
        for (int i = 0; i < 16; ++i)
            if (padButtons[i]->getBounds().contains(x, y)) { if (onSampleDropped) onSampleDropped(i, file); selectPad(i); return; }
        if (selectedPad >= 0 && onSampleDropped) onSampleDropped(selectedPad, file);
    }
    bool isInterestedInDragSource(const juce::DragAndDropTarget::SourceDetails& d) override {
        return d.description.toString()=="FileBrowserDrag"||dynamic_cast<juce::FileTreeComponent*>(d.sourceComponent.get())!=nullptr;
    }
    void itemDropped(const juce::DragAndDropTarget::SourceDetails& d) override {
        if (auto* t = dynamic_cast<juce::FileTreeComponent*>(d.sourceComponent.get())) {
            juce::File f = t->getSelectedFile(0);
            auto e = f.getFileExtension().toLowerCase();
            if (f.existsAsFile() && (e==".wav"||e==".aiff"||e==".aif"||e==".flac"||e==".mp3"||e==".ogg")) {
                juce::StringArray a; a.add(f.getFullPathName());
                filesDropped(a, d.localPosition.x, d.localPosition.y);
            }
        }
    }

    std::function<void(int)>              onPadSelected;
    std::function<void(int, juce::File)>  onSampleDropped;

private:
    static constexpr int kTH = 30, kPadAreaW = 260, kWaveH = 100;

    void updatePadColors() {
        for (int i = 0; i < 16; ++i) {
            padButtons[i]->setColour(juce::TextButton::buttonColourId,
                i == selectedPad ? juce::Colour(0xff5A2A00) : juce::Colour(0xff111118));
            padButtons[i]->setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xffFF6600));
            padButtons[i]->setColour(juce::TextButton::textColourOffId,
                i == selectedPad ? juce::Colour(0xffFF8833) : juce::Colour(0xff666677));
        }
    }

    DrumRackProcessor* processor;
    LiBeLookAndFeel laf;
    int selectedPad = 0;

    std::vector<std::unique_ptr<juce::TextButton>> padButtons;
    LiBeKnob kGain, kPan, kPitch, kDecay;

    juce::AudioFormatManager formatManager;
    juce::AudioThumbnailCache thumbnailCache{5};
    std::unique_ptr<juce::AudioThumbnail> thumbnail;
    juce::Rectangle<int> waveformBounds;
    juce::File lastFile;
};
