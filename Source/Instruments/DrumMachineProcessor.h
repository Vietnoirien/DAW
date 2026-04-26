#pragma once
#include <JuceHeader.h>
#include <atomic>
#include <array>
#include <cmath>
#include "Instrument.h"

// ─── DrumVoiceParams ─────────────────────────────────────────────────────────
// Per-voice parameters, accessed safely from both audio and UI threads.
struct DrumVoiceParams {
    std::atomic<float> p1 { 0.5f }; // primary param (pitch, tone...)
    std::atomic<float> p2 { 0.5f }; // secondary param (decay...)
    std::atomic<float> p3 { 0.5f }; // tertiary param (click, snap...)
    std::atomic<float> p4 { 0.5f }; // quaternary param (sweep, spread...)
    std::atomic<float> level { 0.8f };
};

// ─── Base DrumVoice ──────────────────────────────────────────────────────────
class DrumVoice {
public:
    virtual ~DrumVoice() = default;
    virtual void prepare(double sr) { sampleRate = sr; }
    virtual void trigger(float velocity) = 0;
    virtual float nextSample() = 0;
    void setParams(DrumVoiceParams* p) { params = p; }

    // Trigger flag polled by the editor for the LED indicator
    std::atomic<bool> triggered { false };

protected:
    double sampleRate { 44100.0 };
    DrumVoiceParams* params { nullptr };

    // Lightweight LCG noise — no juce::Random member to avoid LeakedObjectDetector
    float noise() {
        noiseSeed = noiseSeed * 1664525u + 1013904223u;
        return (float)(int32_t(noiseSeed)) / float(0x7fffffffu);
    }
    uint32_t noiseSeed { 12345u };

    // Single-pole HPF
    float hpfState { 0.0f }, hpfPrev { 0.0f };
    float hpf(float x, float cutHz) {
        float rc    = 1.0f / (juce::MathConstants<float>::twoPi * cutHz);
        float alpha = rc / (rc + 1.0f / float(sampleRate));
        hpfState    = alpha * (hpfState + x - hpfPrev);
        hpfPrev     = x;
        return hpfState;
    }
};

// ─── KickVoice ───────────────────────────────────────────────────────────────
// p1=pitch(40-80Hz) p2=decay(0.2-2s) p3=click(0-1) p4=pitchSweep(0-1)
class KickVoice : public DrumVoice {
public:
    void trigger(float velocity) override {
        vel = velocity; t = 0.0f; phase = 0.0f; active = true;
        triggered.store(true, std::memory_order_relaxed);
    }
    float nextSample() override {
        if (!active) return 0.0f;
        float dt = 1.0f / float(sampleRate);
        float fEnd   = 30.0f + params->p1.load() * 50.0f;    // 30..80 Hz
        float fStart = fEnd + 20.0f + params->p4.load() * 280.0f; // sweep
        float tDecay = 0.15f + params->p2.load() * 1.85f;    // 0.15..2s
        float clickAmt = params->p3.load();

        float freq = fEnd + (fStart - fEnd) * std::exp(-t * 30.0f);
        phase += freq * dt;
        float body = std::sin(juce::MathConstants<float>::twoPi * phase);
        float ampEnv = std::exp(-t / tDecay);
        // Click: short noise burst
        float click = (t < 0.012f) ? noise() * std::exp(-t * 600.0f) * clickAmt : 0.0f;
        // Soft clip
        float out = std::tanh((body * ampEnv + click) * 1.5f) * vel * params->level.load();
        t += dt;
        if (t > tDecay * 6.0f) active = false;
        return out;
    }
private:
    float t { 0.0f }, phase { 0.0f }, vel { 1.0f };
    bool active { false };
};

// ─── SnareVoice ──────────────────────────────────────────────────────────────
// p1=tone(150-300Hz) p2=decay(0.05-0.5s) p3=snap(0-1) p4=tune(detune)
class SnareVoice : public DrumVoice {
public:
    void trigger(float velocity) override {
        vel = velocity; t = 0.0f;
        phase1 = 0.0f; phase2 = 0.0f; active = true;
        triggered.store(true, std::memory_order_relaxed);
    }
    float nextSample() override {
        if (!active) return 0.0f;
        float dt = 1.0f / float(sampleRate);
        float f1   = 150.0f + params->p1.load() * 150.0f;
        float f2   = f1 * (1.0f + params->p4.load() * 0.5f + 0.08f); // detune
        float tDec = 0.05f + params->p2.load() * 0.45f;
        float snap  = params->p3.load();

        float bodyEnv = std::exp(-t / tDec);
        float noiseEnv = std::exp(-t / (tDec * 1.4f));

        phase1 += f1 * dt; phase2 += f2 * dt;
        float body = (std::sin(juce::MathConstants<float>::twoPi * phase1)
                    + std::sin(juce::MathConstants<float>::twoPi * phase2)) * 0.5f;

        // Noise through crude BPF (HPF 300Hz + LPF 8kHz approx)
        float n = noise();
        noiseHP = 0.95f * (noiseHP + n - noisePrev); noisePrev = n;

        float noiseLayer = noiseHP * snap;
        float out = (body * bodyEnv + noiseLayer * noiseEnv) * vel * params->level.load();
        out = std::tanh(out * 1.2f);
        t += dt;
        if (t > tDec * 8.0f) active = false;
        return out;
    }
private:
    float t{}, phase1{}, phase2{}, vel{1.f};
    float noiseHP{}, noisePrev{};
    bool active{false};
};

// ─── ClosedHatVoice ──────────────────────────────────────────────────────────
// 6-oscillator swarm (TR-808 style) + HPF. p1=decay(5-80ms) p2=tone(cutoff)
class ClosedHatVoice : public DrumVoice {
    static constexpr float kFreqs[6] = {205.f,292.f,413.f,523.f,715.f,1018.f};
public:
    void prepare(double sr) override { DrumVoice::prepare(sr); phases.fill(0.f); }
    void trigger(float velocity) override {
        vel = velocity; t = 0.f; active = true;
        triggered.store(true, std::memory_order_relaxed);
    }
    float nextSample() override {
        if (!active) return 0.f;
        float dt = 1.f / float(sampleRate);
        float tDec = 0.005f + params->p1.load() * 0.075f;  // 5-80ms
        float cutoff = 4000.f + params->p2.load() * 8000.f;

        float osc = 0.f;
        for (int i = 0; i < 6; ++i) {
            osc += (phases[i] < 0.5f ? 1.f : -1.f); // square
            phases[i] = std::fmod(phases[i] + kFreqs[i] * dt, 1.f);
        }
        osc /= 6.f;
        osc = hpf(osc, cutoff);
        float out = osc * std::exp(-t / tDec) * vel * params->level.load();
        t += dt;
        if (t > tDec * 8.f) active = false;
        return out;
    }
    std::array<float, 6> phases {};
private:
    float t{}, vel{1.f};
    bool active{false};
};
constexpr float ClosedHatVoice::kFreqs[6];

// ─── OpenHatVoice ────────────────────────────────────────────────────────────
// Same swarm, longer decay. p1=decay(100ms-1s). Chokeable.
class OpenHatVoice : public DrumVoice {
    static constexpr float kFreqs[6] = {205.f,292.f,413.f,523.f,715.f,1018.f};
public:
    void prepare(double sr) override { DrumVoice::prepare(sr); phases.fill(0.f); }
    void trigger(float velocity) override {
        vel = velocity; t = 0.f; active = true;
        triggered.store(true, std::memory_order_relaxed);
    }
    void choke() { active = false; }
    float nextSample() override {
        if (!active) return 0.f;
        float dt = 1.f / float(sampleRate);
        float tDec = 0.1f + params->p1.load() * 0.9f;
        float cutoff = 4000.f + params->p2.load() * 8000.f;
        float osc = 0.f;
        for (int i = 0; i < 6; ++i) {
            osc += (phases[i] < 0.5f ? 1.f : -1.f);
            phases[i] = std::fmod(phases[i] + kFreqs[i] * dt, 1.f);
        }
        osc = hpf(osc / 6.f, cutoff);
        float out = osc * std::exp(-t / tDec) * vel * params->level.load();
        t += dt;
        if (t > tDec * 8.f) active = false;
        return out;
    }
    std::array<float, 6> phases {};
private:
    float t{}, vel{1.f};
    bool active{false};
};
constexpr float OpenHatVoice::kFreqs[6];

// ─── ClapVoice ───────────────────────────────────────────────────────────────
// 4 micro-delayed noise bursts. p1=decay p2=spread(ms) p3=tone
class ClapVoice : public DrumVoice {
public:
    void trigger(float velocity) override {
        vel = velocity; t = 0.f; active = true;
        triggered.store(true, std::memory_order_relaxed);
    }
    float nextSample() override {
        if (!active) return 0.f;
        float dt = 1.f / float(sampleRate);
        float tDec = 0.05f + params->p1.load() * 0.35f;
        float spread = 0.001f + params->p2.load() * 0.029f; // 1-30ms spacing
        float cutLo = 200.f + params->p3.load() * 600.f;

        // 4 burst offsets
        float burst = 0.f;
        const float offsets[4] = {0.f, spread, spread*2.f, spread*3.f};
        for (float off : offsets) {
            float bt = t - off;
            if (bt >= 0.f && bt < 0.02f)
                burst += noise() * std::exp(-bt * 300.f);
        }
        burst = hpf(burst, cutLo) * 0.5f;
        float tailT = t - spread * 3.f;
        float tail = (tailT > 0.f) ? noise() * std::exp(-tailT / tDec) : 0.f;
        tail = hpf(tail, cutLo);
        float out = (burst + tail) * vel * params->level.load();
        t += dt;
        if (t > tDec * 10.f) active = false;
        return out;
    }
private:
    float t{}, vel{1.f};
    bool active{false};
};

// ─── TomVoice ────────────────────────────────────────────────────────────────
// Pitched sine + pitch sweep. p1=pitch(60-400Hz) p2=decay p3=pitchSweep
class TomVoice : public DrumVoice {
public:
    void trigger(float velocity) override {
        vel = velocity; t = 0.f; phase = 0.f; active = true;
        triggered.store(true, std::memory_order_relaxed);
    }
    float nextSample() override {
        if (!active) return 0.f;
        float dt = 1.f / float(sampleRate);
        float fEnd   = 60.f + params->p1.load() * 340.f;
        float fStart = fEnd + params->p3.load() * fEnd;
        float tDec   = 0.1f + params->p2.load() * 0.9f;
        float freq   = fEnd + (fStart - fEnd) * std::exp(-t * 25.f);
        phase += freq * dt;
        float out = std::sin(juce::MathConstants<float>::twoPi * phase)
                  * std::exp(-t / tDec) * vel * params->level.load();
        t += dt;
        if (t > tDec * 6.f) active = false;
        return out;
    }
private:
    float t{}, phase{}, vel{1.f};
    bool active{false};
};

// ─── RimVoice ────────────────────────────────────────────────────────────────
// Short sine click + noise tick. p1=decay p2=tone
class RimVoice : public DrumVoice {
public:
    void trigger(float velocity) override {
        vel = velocity; t = 0.f; phase = 0.f; active = true;
        triggered.store(true, std::memory_order_relaxed);
    }
    float nextSample() override {
        if (!active) return 0.f;
        float dt = 1.f / float(sampleRate);
        float tDec = 0.01f + params->p1.load() * 0.09f;
        float freq  = 800.f + params->p2.load() * 1200.f;
        phase += freq * dt;
        float body = std::sin(juce::MathConstants<float>::twoPi * phase) * std::exp(-t / tDec);
        float tick  = noise() * std::exp(-t / (tDec * 0.3f));
        t += dt;
        if (t > tDec * 8.f) active = false;
        return (body * 0.6f + tick * 0.4f) * vel * params->level.load();
    }
private:
    float t{}, phase{}, vel{1.f};
    bool active{false};
};

// ─── CowbellVoice ────────────────────────────────────────────────────────────
// Two square waves at inharmonic intervals (TR-808 cowbell). p1=decay p2=tone
class CowbellVoice : public DrumVoice {
public:
    void trigger(float velocity) override {
        vel = velocity; t = 0.f; ph1 = 0.f; ph2 = 0.f; active = true;
        triggered.store(true, std::memory_order_relaxed);
    }
    float nextSample() override {
        if (!active) return 0.f;
        float dt = 1.f / float(sampleRate);
        float tDec = 0.08f + params->p1.load() * 1.9f;
        float base  = 540.f + params->p2.load() * 200.f;
        ph1 += base * dt;
        ph2 += base * 1.4717f * dt; // inharmonic ratio
        float osc = ((ph1 < 0.5f ? 1.f : -1.f) + (ph2 < 0.5f ? 1.f : -1.f)) * 0.5f;
        ph1 = std::fmod(ph1, 1.f); ph2 = std::fmod(ph2, 1.f);
        // Bandpass-ish: HPF to remove DC, brief resonance
        float bp = hpf(osc, 400.f);
        float out = bp * std::exp(-t / tDec) * vel * params->level.load();
        t += dt;
        if (t > tDec * 8.f) active = false;
        return out;
    }
private:
    float t{}, ph1{}, ph2{}, vel{1.f};
    bool active{false};
};

// ─── GM MIDI note map ────────────────────────────────────────────────────────
enum DrumVoiceId {
    kKick = 0, kSnare, kClosedHat, kOpenHat, kClap, kTom, kRim, kCowbell,
    kNumVoices
};

static constexpr int kGMNotes[kNumVoices] = {
    36, // Kick
    38, // Snare
    42, // Closed HH
    46, // Open HH
    39, // Clap (hand)
    47, // Mid Tom
    37, // Rim Shot
    56  // Cowbell
};

// ─── DrumMachineProcessor ────────────────────────────────────────────────────
class DrumMachineProcessor : public InstrumentProcessor {
public:
    DrumMachineProcessor();
    ~DrumMachineProcessor() override = default;

    void prepareToPlay(double sampleRate) override;
    void processBlock(juce::AudioBuffer<float>& outBuffer,
                      const juce::MidiBuffer& midi) override;
    void clear() override;

    juce::ValueTree saveState() const override;
    void loadState(const juce::ValueTree& tree) override;
    void registerAutomationParameters(AutomationRegistry* registry) override;
    std::unique_ptr<juce::Component> createEditor() override;
    juce::String getName() const override { return "DrumMachine"; }

    // Per-voice params, indexed by DrumVoiceId
    std::array<DrumVoiceParams, kNumVoices> voiceParams;

    // Waveform display FIFO (processor-level, not per-voice)
    static constexpr int kFifoSize = 4096;
    juce::AbstractFifo displayFifo { kFifoSize };
    std::array<float, kFifoSize> displayData {};

    // Which voice the editor has selected — used for fallback MIDI mapping
    std::atomic<int> selectedVoice { 0 };

    DrumVoice* voicePtrs[kNumVoices] {};

    std::unique_ptr<KickVoice>       kick;
    std::unique_ptr<SnareVoice>      snare;
    std::unique_ptr<ClosedHatVoice>  closedHat;
    std::unique_ptr<OpenHatVoice>    openHat;
    std::unique_ptr<ClapVoice>       clap;
    std::unique_ptr<TomVoice>        tom;
    std::unique_ptr<RimVoice>        rim;
    std::unique_ptr<CowbellVoice>    cowbell;
};
