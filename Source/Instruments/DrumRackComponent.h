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
//  │                      │  [M][S]  PAD CONTROLS                               │
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

        // 4x4 pad buttons — support right-click via a small wrapper
        for (int i = 0; i < 16; ++i) {
            auto btn = std::make_unique<PadButton>(juce::String(i + 1));
            btn->onClick          = [this, i] { selectPad(i); };
            btn->onRightClick     = [this, i] { showPadContextMenu(i); };
            addAndMakeVisible(*btn);
            padButtons.push_back(std::move(btn));
        }

        // ── Mute button ──
        muteBtn.setButtonText("M");
        muteBtn.setClickingTogglesState(true);
        muteBtn.setColour(juce::TextButton::buttonColourId,   juce::Colour(0xff1A1020));
        muteBtn.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xffCC2255));
        muteBtn.setColour(juce::TextButton::textColourOffId,  juce::Colour(0xff888899));
        muteBtn.setColour(juce::TextButton::textColourOnId,   juce::Colours::white);
        muteBtn.onClick = [this] {
            if (selectedPad >= 0) {
                bool nowMuted = muteBtn.getToggleState();
                processor->settings[selectedPad].muted.store(nowMuted);
                // Mute cancels solo
                if (nowMuted) {
                    processor->settings[selectedPad].soloed.store(false);
                    soloBtn.setToggleState(false, juce::dontSendNotification);
                }
                updatePadColors();
            }
        };
        addAndMakeVisible(muteBtn);

        // ── Solo button ──
        soloBtn.setButtonText("S");
        soloBtn.setClickingTogglesState(true);
        soloBtn.setColour(juce::TextButton::buttonColourId,   juce::Colour(0xff1A1008));
        soloBtn.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xffFF8C00));
        soloBtn.setColour(juce::TextButton::textColourOffId,  juce::Colour(0xff888899));
        soloBtn.setColour(juce::TextButton::textColourOnId,   juce::Colours::white);
        soloBtn.onClick = [this] {
            if (selectedPad >= 0) {
                bool nowSoloed = soloBtn.getToggleState();
                processor->settings[selectedPad].soloed.store(nowSoloed);
                // Solo unmutes this pad
                if (nowSoloed) {
                    processor->settings[selectedPad].muted.store(false);
                    muteBtn.setToggleState(false, juce::dontSendNotification);
                }
                updatePadColors();
            }
        };
        addAndMakeVisible(soloBtn);

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

        // Right panel: waveform + controls
        auto right = b.reduced(6, 4);
        waveformBounds = right.removeFromTop(kWaveH);
        right.removeFromTop(6); // breathing room

        // Mute / Solo row + label
        auto msRow = right.removeFromTop(22);
        muteBtn.setBounds(msRow.removeFromLeft(36));
        msRow.removeFromLeft(4);
        soloBtn.setBounds(msRow.removeFromLeft(36));

        right.removeFromTop(8); // gap before knobs

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

        // "PAD CONTROLS" section label (next to M/S buttons)
        g.setColour(juce::Colour(0xffFF6600).withAlpha(0.65f));
        g.setFont(juce::FontOptions(9.0f, juce::Font::bold));
        int msY = waveformBounds.getBottom() + 10;
        g.drawText("PAD CONTROLS", kPadAreaW + 86, msY, 160, 22, juce::Justification::centredLeft);
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

        // Sync mute/solo toggle states
        muteBtn.setToggleState(processor->settings[idx].muted.load(),  juce::dontSendNotification);
        soloBtn.setToggleState(processor->settings[idx].soloed.load(), juce::dontSendNotification);

        juce::String ps = "DrumRack/Pad " + juce::String(idx+1) + "/";
        kGain.slider.getProperties().set ("parameterId", ps + "Gain");
        kPan.slider.getProperties().set  ("parameterId", ps + "Pan");
        kPitch.slider.getProperties().set("parameterId", ps + "Pitch");
        kDecay.slider.getProperties().set("parameterId", ps + "Decay");

        updatePadColors();
        if (onPadSelected) onPadSelected(idx);
        repaint();
    }

    // ─── Right-click context menu ──────────────────────────────────────────────
    void showPadContextMenu(int padIdx) {
        selectPad(padIdx);

        juce::PopupMenu menu;
        menu.setLookAndFeel(&laf);

        bool hasSample = processor->settings[padIdx].loadedFile.existsAsFile();
        bool isMuted   = processor->settings[padIdx].muted.load();
        bool isSoloed  = processor->settings[padIdx].soloed.load();

        menu.addSectionHeader("Pad " + juce::String(padIdx + 1));
        menu.addItem(1, "Remove Sample", hasSample);
        menu.addSeparator();
        menu.addItem(2, isMuted  ? "Unmute" : "Mute");
        menu.addItem(3, isSoloed ? "Unsolo" : "Solo");

        menu.showMenuAsync(juce::PopupMenu::Options{}.withTargetComponent(padButtons[padIdx].get()),
            [this, padIdx](int result) {
                switch (result) {
                    case 1: // Remove sample
                        processor->removeSample(padIdx);
                        updatePadColors();
                        repaint();
                        break;
                    case 2: { // Toggle mute
                        bool nowMuted = !processor->settings[padIdx].muted.load();
                        processor->settings[padIdx].muted.store(nowMuted);
                        if (nowMuted) processor->settings[padIdx].soloed.store(false);
                        if (padIdx == selectedPad) {
                            muteBtn.setToggleState(nowMuted, juce::dontSendNotification);
                            soloBtn.setToggleState(false,    juce::dontSendNotification);
                        }
                        updatePadColors();
                        break;
                    }
                    case 3: { // Toggle solo
                        bool nowSoloed = !processor->settings[padIdx].soloed.load();
                        processor->settings[padIdx].soloed.store(nowSoloed);
                        if (nowSoloed) processor->settings[padIdx].muted.store(false);
                        if (padIdx == selectedPad) {
                            soloBtn.setToggleState(nowSoloed, juce::dontSendNotification);
                            muteBtn.setToggleState(false,     juce::dontSendNotification);
                        }
                        updatePadColors();
                        break;
                    }
                    default: break;
                }
            });
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
    // ─── PadButton: TextButton that also fires onRightClick ───────────────────
    // Right-click must be caught in mouseDown (isPopupMenu) because by the time
    // mouseUp fires the button flag has already been cleared from e.mods.
    struct PadButton : public juce::TextButton {
        using juce::TextButton::TextButton;
        std::function<void()> onRightClick;

        void mouseDown(const juce::MouseEvent& e) override {
            if (e.mods.isPopupMenu()) {
                if (onRightClick) onRightClick();
                return; // don't let TextButton arm its click logic
            }
            juce::TextButton::mouseDown(e);
        }

        void mouseUp(const juce::MouseEvent& e) override {
            if (!e.mods.isPopupMenu())
                juce::TextButton::mouseUp(e);
        }
    };

    static constexpr int kTH = 30, kPadAreaW = 260, kWaveH = 100;

    void updatePadColors() {
        bool hasSolo = processor->anySoloed();

        for (int i = 0; i < 16; ++i) {
            bool isMuted  = processor->settings[i].muted.load();
            bool isSoloed = processor->settings[i].soloed.load();
            // A pad is "silenced" if muted, OR if any pad is soloed and this one isn't
            bool silenced = isMuted || (hasSolo && !isSoloed);

            juce::Colour base = (i == selectedPad) ? juce::Colour(0xff5A2A00)
                                                    : juce::Colour(0xff111118);
            if (silenced)  base = base.darker(0.55f);
            if (isSoloed)  base = base.interpolatedWith(juce::Colour(0xff6B3A00), 0.5f);

            padButtons[i]->setColour(juce::TextButton::buttonColourId,  base);
            padButtons[i]->setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xffFF6600));

            juce::Colour textCol = silenced  ? juce::Colour(0xff333344)
                                 : isSoloed  ? juce::Colour(0xffFFAA44)
                                 : (i == selectedPad) ? juce::Colour(0xffFF8833)
                                 : juce::Colour(0xff666677);
            padButtons[i]->setColour(juce::TextButton::textColourOffId, textCol);
        }
    }

    DrumRackProcessor* processor;
    LiBeLookAndFeel laf;
    int selectedPad = 0;

    std::vector<std::unique_ptr<PadButton>> padButtons;
    juce::TextButton muteBtn, soloBtn;
    LiBeKnob kGain, kPan, kPitch, kDecay;

    juce::AudioFormatManager formatManager;
    juce::AudioThumbnailCache thumbnailCache{5};
    std::unique_ptr<juce::AudioThumbnail> thumbnail;
    juce::Rectangle<int> waveformBounds;
    juce::File lastFile;
};
