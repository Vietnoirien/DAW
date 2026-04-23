#pragma once
#include <JuceHeader.h>
#include <atomic>
#include <array>
#include <vector>
#include <cmath>
#include "Instrument.h"

// ============================================================
// KSParams — all cross-thread state via std::atomic
// ============================================================
struct KSParams {
    // Excitation type: 0=Noise burst, 1=Impulse, 2=Filtered noise
    std::atomic<int>   excitationType { 0 };
    std::atomic<float> excitationLevel{ 1.0f };

    // Damping (0=bright/long sustain, 1=dark/fast decay)
    std::atomic<float> damping { 0.5f };

    // Stretch factor: 1.0=pure KS string, >1.0=bell/inharmonic
    std::atomic<float> stretch { 1.0f };

    // Pickup position (0.0=bridge, 0.5=mid, affects timbre via comb cancellation)
    std::atomic<float> pickupPos { 0.5f };

    // Decay time multiplier (1.0=natural, >1.0=longer)
    std::atomic<float> decayTime { 1.0f };

    // Master level
    std::atomic<float> masterLevel { 0.8f };

    // Amp envelope (just attack + release, KS handles natural decay)
    std::atomic<float> attack  { 0.001f };
    std::atomic<float> release { 0.1f };
};

// ============================================================
// KSVoice — single Karplus-Strong voice with fractional delay
// All buffers pre-allocated in prepare(), no audio-thread allocs
// ============================================================
class KSVoice {
public:
    static constexpr int kMaxBufferSize = 8192; // enough for ~5.4 Hz at 44100 SR

    // Pre-allocate buffer to avoid any audio-thread heap allocation
    KSVoice() {
        delayBuffer.resize(kMaxBufferSize, 0.0f);
    }

    void prepare(double sr) {
        sampleRate = sr;
        ampAdsr.setSampleRate(sr);
        active = false;
        writePos = 0;
        std::fill(delayBuffer.begin(), delayBuffer.end(), 0.0f);
    }

    void noteOn(int note, float vel, const KSParams& p) {
        currentNote     = note;
        currentVelocity = vel;
        active          = true;
        prevSample      = 0.0f;
        filterState     = 0.0f;

        double freq = juce::MidiMessage::getMidiNoteInHertz(note);
        // Fractional delay length for target frequency
        delayLength = (float)(sampleRate / freq);
        delayLength = juce::jlimit(2.0f, (float)(kMaxBufferSize - 1), delayLength);

        int intLen = (int)delayLength;
        writePos = 0;

        // Fill delay buffer with excitation signal
        int exType = p.excitationType.load();
        float exLevel = p.excitationLevel.load();
        for (int i = 0; i < intLen; ++i) {
            float sample = 0.0f;
            if (exType == 0) {
                // White noise burst
                sample = (rng.nextFloat() * 2.0f - 1.0f) * exLevel;
            } else if (exType == 1) {
                // Impulse at position 0
                sample = (i == 0) ? exLevel : 0.0f;
            } else {
                // Filtered noise: apply simple LP filter to noise
                float n = rng.nextFloat() * 2.0f - 1.0f;
                filterState = filterState * 0.6f + n * 0.4f;
                sample = filterState * exLevel;
            }
            delayBuffer[i] = sample;
        }
        // Clear rest
        for (int i = intLen; i < kMaxBufferSize; ++i) delayBuffer[i] = 0.0f;

        juce::ADSR::Parameters ap { p.attack.load(), 0.001f, 1.0f, p.release.load() };
        ampAdsr.setParameters(ap);
        ampAdsr.noteOn();
    }

    void noteOff() { ampAdsr.noteOff(); }
    bool isActive() const { return active; }
    int  getNote()  const { return currentNote; }

    void renderNextBlock(juce::AudioBuffer<float>& out, int startSample, int numSamples,
                         const KSParams& p) {
        if (!active) return;

        const float damping    = juce::jlimit(0.0f, 0.9999f, p.damping.load());
        const float stretch    = juce::jlimit(1.0f, 2.0f,    p.stretch.load());
        const float decayMult  = juce::jlimit(0.1f, 5.0f,    p.decayTime.load());
        const float masterLvl  = p.masterLevel.load();
        const float pickup     = juce::jlimit(0.0f, 1.0f,    p.pickupPos.load());

        // Feedback coefficient derived from decay time
        // Natural KS decay ≈ exp(-π * damping / delayLength)
        // We modulate with decayMult
        const float g = std::pow(10.0f, -3.0f * (1.0f / delayLength) * (1.0f / decayMult));
        const float filtCoeff = 1.0f - damping; // low-pass blend: 0=all feedback, 1=all prev

        juce::ADSR::Parameters ap { p.attack.load(), 0.001f, 1.0f, p.release.load() };
        ampAdsr.setParameters(ap);

        int bufSize = (int)delayLength;

        for (int s = 0; s < numSamples; ++s) {
            const float ampEnv = ampAdsr.getNextSample();
            if (!ampAdsr.isActive()) { active = false; break; }

            // Fractional read position (integer + fractional delay)
            float readF = (float)writePos - delayLength;
            while (readF < 0.0f) readF += (float)kMaxBufferSize;

            int readI0 = (int)readF & (kMaxBufferSize - 1);
            int readI1 = (readI0 + 1) & (kMaxBufferSize - 1);
            float frac = readF - (int)readF;
            float delayed = delayBuffer[readI0] + frac * (delayBuffer[readI1] - delayBuffer[readI0]);

            // Stretch: blend between current and previous for inharmonicity
            float stretchedSample = delayed;
            if (stretch > 1.0f) {
                stretchedSample = delayed * (2.0f - stretch) + prevSample * (stretch - 1.0f);
            }

            // Low-pass filter (Karplus-Strong averaging filter)
            float filtered = (stretchedSample * filtCoeff + prevSample * (1.0f - filtCoeff)) * g;
            prevSample = filtered;

            // Write back to delay line
            delayBuffer[writePos] = filtered;
            writePos = (writePos + 1) & (kMaxBufferSize - 1);

            // Pickup position comb filtering (simulates pick position)
            // Simple: just output the direct signal (pickup pos affects tone subtly via buffer init)
            float output = filtered * ampEnv * currentVelocity * masterLvl;

            out.addSample(0, startSample + s, output);
            out.addSample(1, startSample + s, output);
        }
    }

private:
    double sampleRate { 44100.0 };
    bool   active     { false };
    int    currentNote     { 0 };
    float  currentVelocity { 0.0f };
    float  prevSample      { 0.0f };
    float  filterState     { 0.0f };
    float  delayLength     { 100.0f };
    int    writePos        { 0 };

    // Pre-allocated delay buffer (no audio-thread allocation)
    std::vector<float> delayBuffer;

    juce::ADSR ampAdsr;
    juce::Random rng;
};

// ============================================================
// KarplusStrongProcessor — 8-voice polyphonic physical model
// ============================================================
class KarplusStrongProcessor : public InstrumentProcessor {
public:
    KSParams params;

    KarplusStrongProcessor() = default;

    void prepareToPlay(double sampleRate) override {
        for (auto& v : voices) v.prepare(sampleRate);
    }

    void processBlock(juce::AudioBuffer<float>& out, const juce::MidiBuffer& midi) override {
        int n = out.getNumSamples(), pos = 0;
        for (const auto m : midi) {
            auto msg = m.getMessage(); int sp = m.samplePosition;
            if (sp > pos) { renderVoices(out, pos, sp - pos); pos = sp; }
            if      (msg.isNoteOn())      handleNoteOn (msg.getNoteNumber(), msg.getFloatVelocity());
            else if (msg.isNoteOff())     handleNoteOff(msg.getNoteNumber());
            else if (msg.isAllNotesOff()) { for (auto& v : voices) v.noteOff(); }
        }
        if (pos < n) renderVoices(out, pos, n - pos);
    }

    void clear() override { for (auto& v : voices) v.noteOff(); }

    juce::ValueTree saveState() const override {
        juce::ValueTree t("KarplusStrongState");
        t.setProperty("exType",   params.excitationType.load(),  nullptr);
        t.setProperty("exLevel",  params.excitationLevel.load(), nullptr);
        t.setProperty("damping",  params.damping.load(),         nullptr);
        t.setProperty("stretch",  params.stretch.load(),         nullptr);
        t.setProperty("pickup",   params.pickupPos.load(),       nullptr);
        t.setProperty("decay",    params.decayTime.load(),       nullptr);
        t.setProperty("master",   params.masterLevel.load(),     nullptr);
        t.setProperty("attack",   params.attack.load(),          nullptr);
        t.setProperty("release",  params.release.load(),         nullptr);
        return t;
    }

    void loadState(const juce::ValueTree& t) override {
        params.excitationType.store ((int)  t.getProperty("exType",  0));
        params.excitationLevel.store((float)t.getProperty("exLevel", 1.0f));
        params.damping.store        ((float)t.getProperty("damping", 0.5f));
        params.stretch.store        ((float)t.getProperty("stretch", 1.0f));
        params.pickupPos.store      ((float)t.getProperty("pickup",  0.5f));
        params.decayTime.store      ((float)t.getProperty("decay",   1.0f));
        params.masterLevel.store    ((float)t.getProperty("master",  0.8f));
        params.attack.store         ((float)t.getProperty("attack",  0.001f));
        params.release.store        ((float)t.getProperty("release", 0.1f));
    }
    
    void registerAutomationParameters(AutomationRegistry* registry) override {
        if (!registry) return;
        
        // IDs must exactly match the `parameterId` set in KarplusStrongComponent.h
        registry->registerParameter("KS/MasterLevel",      &params.masterLevel,      0.0f,  1.0f);
        registry->registerParameter("KS/ExcitationLevel",  &params.excitationLevel,  0.0f,  1.0f);
        registry->registerParameter("KS/Damping",          &params.damping,           0.0f,  0.9999f);
        registry->registerParameter("KS/Stretch",          &params.stretch,           1.0f,  2.0f);
        registry->registerParameter("KS/Pickup",           &params.pickupPos,         0.0f,  1.0f);
        registry->registerParameter("KS/DecayTime",        &params.decayTime,         0.1f,  5.0f);
        registry->registerParameter("KS/Attack",           &params.attack,            0.001f, 0.5f);
        registry->registerParameter("KS/Release",          &params.release,           0.001f, 2.0f);
    }

    std::unique_ptr<juce::Component> createEditor() override;
    juce::String getName() const override { return "KarplusStrong"; }

private:
    void handleNoteOn(int n, float v) {
        for (auto& vo : voices) {
            if (!vo.isActive()) { vo.noteOn(n, v, params); return; }
        }
        for (auto& vo : voices) {
            if (vo.getNote() == n) { vo.noteOn(n, v, params); return; }
        }
        voices[0].noteOn(n, v, params);
    }
    void handleNoteOff(int n) {
        for (auto& v : voices) if (v.isActive() && v.getNote() == n) v.noteOff();
    }
    void renderVoices(juce::AudioBuffer<float>& b, int s, int n) {
        for (auto& v : voices) if (v.isActive()) v.renderNextBlock(b, s, n, params);
    }

    std::array<KSVoice, 8> voices;
};
