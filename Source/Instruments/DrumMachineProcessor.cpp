#include "DrumMachineProcessor.h"
#include "../UI/LiBeLookAndFeel.h"

// ─── Helpers ─────────────────────────────────────────────────────────────────
static constexpr const char* kVoiceNames[kNumVoices] = {
    "Kick", "Snare", "Closed HH", "Open HH",
    "Clap", "Tom", "Rim Shot", "Cowbell"
};

// Param labels per voice: [voice][param index 0..3]
static constexpr const char* kParamLabels[kNumVoices][4] = {
    { "Pitch",  "Decay",   "Click",  "Sweep"  }, // Kick
    { "Tone",   "Decay",   "Snap",   "Tune"   }, // Snare
    { "Decay",  "Tone",    "",       ""       }, // CHH
    { "Decay",  "Tone",    "",       ""       }, // OHH
    { "Decay",  "Spread",  "Tone",   ""       }, // Clap
    { "Pitch",  "Decay",   "Sweep",  ""       }, // Tom
    { "Decay",  "Tone",    "",       ""       }, // Rim
    { "Decay",  "Tone",    "",       ""       }, // Cowbell
};
static constexpr int kParamCount[kNumVoices] = { 4, 4, 2, 2, 3, 3, 2, 2 };

// ─── DrumMachineProcessor ────────────────────────────────────────────────────
DrumMachineProcessor::DrumMachineProcessor() {
    kick      = std::make_unique<KickVoice>();
    snare     = std::make_unique<SnareVoice>();
    closedHat = std::make_unique<ClosedHatVoice>();
    openHat   = std::make_unique<OpenHatVoice>();
    clap      = std::make_unique<ClapVoice>();
    tom       = std::make_unique<TomVoice>();
    rim       = std::make_unique<RimVoice>();
    cowbell   = std::make_unique<CowbellVoice>();

    voicePtrs[kKick]      = kick.get();
    voicePtrs[kSnare]     = snare.get();
    voicePtrs[kClosedHat] = closedHat.get();
    voicePtrs[kOpenHat]   = openHat.get();
    voicePtrs[kClap]      = clap.get();
    voicePtrs[kTom]       = tom.get();
    voicePtrs[kRim]       = rim.get();
    voicePtrs[kCowbell]   = cowbell.get();

    for (int i = 0; i < kNumVoices; ++i)
        voicePtrs[i]->setParams(&voiceParams[i]);

    // Sensible defaults
    voiceParams[kKick].p1.store(0.4f);   // pitch mid
    voiceParams[kKick].p2.store(0.35f);  // decay mid
    voiceParams[kKick].p3.store(0.5f);   // click mid
    voiceParams[kKick].p4.store(0.5f);   // sweep mid

    voiceParams[kSnare].p1.store(0.4f);
    voiceParams[kSnare].p2.store(0.3f);
    voiceParams[kSnare].p3.store(0.6f);

    voiceParams[kClosedHat].p1.store(0.08f);
    voiceParams[kClosedHat].p2.store(0.5f);

    voiceParams[kOpenHat].p1.store(0.4f);
    voiceParams[kOpenHat].p2.store(0.5f);

    voiceParams[kClap].p1.store(0.3f);
    voiceParams[kClap].p2.store(0.3f);
    voiceParams[kClap].p3.store(0.4f);

    voiceParams[kTom].p1.store(0.3f);
    voiceParams[kTom].p2.store(0.4f);
    voiceParams[kTom].p3.store(0.4f);
}

void DrumMachineProcessor::prepareToPlay(double sampleRate) {
    for (auto* v : voicePtrs) v->prepare(sampleRate);
}

void DrumMachineProcessor::processBlock(juce::AudioBuffer<float>& out, const juce::MidiBuffer& midi) {
    const int numSamples = out.getNumSamples();

    // Process MIDI events sample-accurately
    int pos = 0;
    for (const auto meta : midi) {
        const auto msg = meta.getMessage();
        const int msgPos = juce::jlimit(0, numSamples - 1, meta.samplePosition);

        // Render up to this event
        for (int s = pos; s < msgPos; ++s) {
            float sample = 0.f;
            for (auto* v : voicePtrs) sample += v->nextSample();
            sample = juce::jlimit(-1.f, 1.f, sample);
            out.addSample(0, s, sample);
            out.addSample(1, s, sample);
        }
        pos = msgPos;

        if (msg.isNoteOn()) {
            int note = msg.getNoteNumber();
            float vel = msg.getFloatVelocity();
            int voiceIdx = -1;

            // 1. Exact GM note match
            for (int i = 0; i < kNumVoices; ++i)
                if (note == kGMNotes[i]) { voiceIdx = i; break; }

            // 2. Fallback: piano-roll notes C2-B2 (36-43) mapped linearly
            //    C2=Kick, C#2=Rim, D2=Snare, D#2=Clap, E2=CHH, F2=OHH, F#2=Tom, G2=Cowbell
            if (voiceIdx < 0 && note >= 36 && note <= 43)
                voiceIdx = note - 36;

            // 3. Last resort: use the voice currently selected in the editor dropdown.
            //    The Euclidean sequencer always sends note 60 — this makes it trigger
            //    whichever drum the user has chosen in the UI.
            if (voiceIdx < 0)
                voiceIdx = selectedVoice.load(std::memory_order_relaxed);

            if (voiceIdx == kClosedHat) openHat->choke();
            voicePtrs[voiceIdx]->trigger(vel);
        }
    }

    // Render remainder
    for (int s = pos; s < numSamples; ++s) {
        float sample = 0.f;
        for (auto* v : voicePtrs) sample += v->nextSample();
        sample = juce::jlimit(-1.f, 1.f, sample);
        out.addSample(0, s, sample);
        out.addSample(1, s, sample);
    }

    // Push to display FIFO (mono, from ch 0)
    const float* ch = out.getReadPointer(0);
    int start1, size1, start2, size2;
    displayFifo.prepareToWrite(numSamples, start1, size1, start2, size2);
    if (size1 > 0) std::copy(ch, ch + size1, displayData.begin() + start1);
    if (size2 > 0) std::copy(ch + size1, ch + size1 + size2, displayData.begin() + start2);
    displayFifo.finishedWrite(size1 + size2);
}

void DrumMachineProcessor::clear() {
    // Voices will naturally decay; nothing to do
}

juce::ValueTree DrumMachineProcessor::saveState() const {
    juce::ValueTree tree("DrumMachineState");
    for (int i = 0; i < kNumVoices; ++i) {
        juce::ValueTree vt(juce::String(kVoiceNames[i]).replace(" ", "_"));
        vt.setProperty("p1", voiceParams[i].p1.load(), nullptr);
        vt.setProperty("p2", voiceParams[i].p2.load(), nullptr);
        vt.setProperty("p3", voiceParams[i].p3.load(), nullptr);
        vt.setProperty("p4", voiceParams[i].p4.load(), nullptr);
        vt.setProperty("level", voiceParams[i].level.load(), nullptr);
        tree.addChild(vt, -1, nullptr);
    }
    return tree;
}

void DrumMachineProcessor::loadState(const juce::ValueTree& tree) {
    for (int i = 0; i < kNumVoices; ++i) {
        auto vt = tree.getChildWithName(juce::String(kVoiceNames[i]).replace(" ", "_"));
        if (!vt.isValid()) continue;
        if (vt.hasProperty("p1")) voiceParams[i].p1.store(vt["p1"]);
        if (vt.hasProperty("p2")) voiceParams[i].p2.store(vt["p2"]);
        if (vt.hasProperty("p3")) voiceParams[i].p3.store(vt["p3"]);
        if (vt.hasProperty("p4")) voiceParams[i].p4.store(vt["p4"]);
        if (vt.hasProperty("level")) voiceParams[i].level.store(vt["level"]);
    }
}

void DrumMachineProcessor::registerAutomationParameters(AutomationRegistry* registry) {
    if (!registry) return;
    for (int v = 0; v < kNumVoices; ++v) {
        juce::String base = juce::String("DrumMachine/") + kVoiceNames[v] + "/";
        int n = kParamCount[v];
        std::atomic<float>* ptrs[4] = {
            &voiceParams[v].p1, &voiceParams[v].p2,
            &voiceParams[v].p3, &voiceParams[v].p4
        };
        for (int p = 0; p < n; ++p) {
            if (juce::String(kParamLabels[v][p]).isNotEmpty())
                registry->registerParameter(base + kParamLabels[v][p], ptrs[p], 0.f, 1.f);
        }
        registry->registerParameter(base + "Level", &voiceParams[v].level, 0.f, 1.f);
    }
}

// ─── DrumMachineEditor ───────────────────────────────────────────────────────
class DrumMachineEditor : public juce::Component,
                          private juce::Timer,
                          private juce::ComboBox::Listener {
public:
    DrumMachineEditor(DrumMachineProcessor* proc)
        : processor(proc)
    {
        setLookAndFeel(&laf);

        // Voice selector
        for (int i = 0; i < kNumVoices; ++i)
            voiceSelector.addItem(kVoiceNames[i], i + 1);
        voiceSelector.setSelectedId(1, juce::dontSendNotification);
        voiceSelector.addListener(this);
        voiceSelector.setColour(juce::ComboBox::backgroundColourId, juce::Colour(0xff1A1A2A));
        voiceSelector.setColour(juce::ComboBox::textColourId, juce::Colours::white);
        voiceSelector.setColour(juce::ComboBox::outlineColourId, accent);
        addAndMakeVisible(voiceSelector);

        // Knobs (up to 4)
        for (int i = 0; i < 4; ++i) {
            knobs[i].setLookAndFeel(&laf);
            addChildComponent(knobs[i]);
        }

        buildKnobsForVoice(0);
        setSize(300, 280);
        startTimerHz(30);
    }

    ~DrumMachineEditor() override {
        stopTimer();
        voiceSelector.removeListener(this);
        for (auto& k : knobs) k.setLookAndFeel(nullptr);
        setLookAndFeel(nullptr);
    }

    void paint(juce::Graphics& g) override {
        // Background
        g.fillAll(juce::Colour(0xff0D0D14));
        g.setColour(juce::Colour(0xff141420));
        g.fillRect(0, 0, getWidth(), 28);
        g.setColour(accent);
        g.fillRect(0, 0, 3, 28);
        g.setColour(juce::Colours::white.withAlpha(0.88f));
        g.setFont(juce::Font(juce::FontOptions(13.f, juce::Font::bold)));
        g.drawText("DRUM MACHINE", 8, 0, getWidth() - 80, 28, juce::Justification::centredLeft);

        // LED trigger indicator
        int ledX = getWidth() - 22, ledY = 8;
        g.setColour(ledActive ? accent : juce::Colour(0xff333333));
        g.fillEllipse(ledX, ledY, 12, 12);
        g.setColour(juce::Colour(0xff2A2A3A));
        g.drawRect(getLocalBounds(), 1);

        // Waveform display area
        auto waveArea = getWaveArea();
        g.setColour(juce::Colour(0xff0A0A12));
        g.fillRect(waveArea);
        g.setColour(juce::Colour(0xff222230));
        g.drawRect(waveArea, 1);

        // Draw waveform from FIFO
        if (displayBuf.size() > 0) {
            juce::Path p;
            int n = int(displayBuf.size());
            for (int i = 0; i < waveArea.getWidth(); ++i) {
                int idx = (i * n) / waveArea.getWidth();
                idx = juce::jlimit(0, n-1, idx);
                float y = waveArea.getCentreY() - displayBuf[idx] * waveArea.getHeight() * 0.45f;
                if (i == 0) p.startNewSubPath(waveArea.getX() + i, y);
                else p.lineTo(waveArea.getX() + i, y);
            }
            g.setColour(accent.withAlpha(0.8f));
            g.strokePath(p, juce::PathStrokeType(1.5f));
        }

        // Centre line
        g.setColour(juce::Colour(0x30ffffff));
        g.drawHorizontalLine(waveArea.getCentreY(), float(waveArea.getX()), float(waveArea.getRight()));

        // MIDI note hint
        int voiceIdx = voiceSelector.getSelectedId() - 1;
        if (voiceIdx >= 0 && voiceIdx < kNumVoices) {
            g.setColour(juce::Colours::white.withAlpha(0.35f));
            g.setFont(9.f);
            g.drawText("MIDI " + juce::String(kGMNotes[voiceIdx]),
                       waveArea.getRight() - 55, waveArea.getY() + 4, 50, 10,
                       juce::Justification::right);
        }
    }

    void resized() override {
        voiceSelector.setBounds(10, 32, getWidth() - 20, 26);
        rebuildKnobPositions();
    }

private:
    juce::Rectangle<int> getWaveArea() const {
        return { 10, 64, getWidth() - 20, 80 };
    }

    void comboBoxChanged(juce::ComboBox*) override {
        int idx = voiceSelector.getSelectedId() - 1;
        if (idx >= 0 && idx < kNumVoices) {
            processor->selectedVoice.store(idx, std::memory_order_relaxed);
            buildKnobsForVoice(idx);
        }
    }

    void buildKnobsForVoice(int voiceIdx) {
        currentVoice = voiceIdx;
        int n = kParamCount[voiceIdx];
        std::atomic<float>* ptrs[4] = {
            &processor->voiceParams[voiceIdx].p1,
            &processor->voiceParams[voiceIdx].p2,
            &processor->voiceParams[voiceIdx].p3,
            &processor->voiceParams[voiceIdx].p4
        };
        for (int i = 0; i < 4; ++i) {
            knobs[i].setVisible(i < n && juce::String(kParamLabels[voiceIdx][i]).isNotEmpty());
            if (i < n && juce::String(kParamLabels[voiceIdx][i]).isNotEmpty()) {
                juce::String pid = juce::String("DrumMachine/") + kVoiceNames[voiceIdx]
                                 + "/" + kParamLabels[voiceIdx][i];
                knobs[i].setup(kParamLabels[voiceIdx][i],
                               ptrs[i]->load(), 0.0, 1.0, ptrs[i]->load(), pid, accent);
                auto* ptr = ptrs[i];
                knobs[i].slider.onValueChange = [ptr, this, i]() {
                    ptr->store(float(knobs[i].slider.getValue()), std::memory_order_relaxed);
                };
                knobs[i].startTrackingParam(ptrs[i], 0.f, 1.f);
            }
        }
        rebuildKnobPositions();
    }

    void rebuildKnobPositions() {
        int n = kParamCount[currentVoice];
        int visN = 0;
        for (int i = 0; i < 4; ++i) if (knobs[i].isVisible()) ++visN;
        int totalW = visN * LiBeKnob::kW + (visN - 1) * 10;
        int startX = (getWidth() - totalW) / 2;
        int y = getWaveArea().getBottom() + 14;
        int xi = startX;
        for (int i = 0; i < 4; ++i) {
            if (knobs[i].isVisible()) {
                knobs[i].setBounds(xi, y, LiBeKnob::kW, LiBeKnob::kH);
                xi += LiBeKnob::kW + 10;
            }
        }
        juce::ignoreUnused(n);
    }

    void timerCallback() override {
        // Pull waveform data from FIFO
        int avail = processor->displayFifo.getNumReady();
        if (avail >= 256) {
            displayBuf.resize(avail);
            int s1, n1, s2, n2;
            processor->displayFifo.prepareToRead(avail, s1, n1, s2, n2);
            if (n1 > 0) std::copy(processor->displayData.begin() + s1,
                                  processor->displayData.begin() + s1 + n1,
                                  displayBuf.begin());
            if (n2 > 0) std::copy(processor->displayData.begin() + s2,
                                  processor->displayData.begin() + s2 + n2,
                                  displayBuf.begin() + n1);
            processor->displayFifo.finishedRead(n1 + n2);
        }

        // LED: check if any voice triggered
        bool anyTriggered = false;
        for (auto* v : processor->voicePtrs) {
            if (v->triggered.exchange(false, std::memory_order_relaxed))
                anyTriggered = true;
        }
        if (anyTriggered) { ledActive = true; ledCounter = 8; }
        else if (ledCounter > 0) { --ledCounter; if (ledCounter == 0) ledActive = false; }

        repaint();
    }

    DrumMachineProcessor* processor;
    juce::Colour accent { 0xffFF3D5B };
    LiBeLookAndFeel laf { accent };
    juce::ComboBox voiceSelector;
    LiBeKnob knobs[4];
    int currentVoice { 0 };
    std::vector<float> displayBuf;
    bool ledActive { false };
    int  ledCounter { 0 };
};

std::unique_ptr<juce::Component> DrumMachineProcessor::createEditor() {
    return std::make_unique<DrumMachineEditor>(this);
}
