#pragma once
#include <JuceHeader.h>
#include <atomic>
#include <array>
#include <cmath>
#include "Instrument.h"

// ============================================================
// DX7 Algorithm Table (32 algorithms, mapped to 4 operators)
// Op indices: 0=Op1(carrier), 1=Op2, 2=Op3, 3=Op4(deepest mod)
//
// modulatedBy[i]: bitmask — which ops feed INTO op i as FM
// isCarrier[i]:   true if op i outputs to the audio bus
// ============================================================
struct DX7AlgorithmDef {
    int  modulatedBy[4]; // bit j set => op j's output modulates op i
    bool isCarrier[4];
};

// 32 algorithms faithful to the DX7 spirit, adapted for 4 operators.
// Op 3 = deepest modulator (like DX7 Op6), Op 0 = primary carrier (DX7 Op1).
static constexpr DX7AlgorithmDef kDX7Algorithms[32] = {
    //  Alg 1:  3->2->1->0(C)  [linear chain]
    { {1<<1, 1<<2, 1<<3, 0}, {true,false,false,false} },
    //  Alg 2:  3->2->1->0(C), 3->0(C)  [chain + shortcut to carrier]
    { {1<<1|1<<3, 1<<2, 1<<3, 0}, {true,false,false,false} },
    //  Alg 3:  3->2->0(C), 3->1->0(C)  [branching mod feeds two paths]
    { {1<<1|1<<2, 1<<3, 1<<3, 0}, {true,false,false,false} },
    //  Alg 4:  3->2->0(C), 1->0(C)  [two independent branches into carrier]
    { {1<<1|1<<2, 0, 1<<3, 0}, {true,false,false,false} },
    //  Alg 5:  3->2->0(C), 3->1(C)  [shared mod, dual carriers]
    { {1<<2, 1<<3, 1<<3, 0}, {true,true,false,false} },
    //  Alg 6:  3->2->0(C), 1(C)  [chain + independent bare carrier]
    { {1<<2, 0, 1<<3, 0}, {true,true,false,false} },
    //  Alg 7:  2->1->0(C), 3->0(C)  [chain-of-3 + direct extra mod to carrier]
    { {1<<1|1<<3, 1<<2, 0, 0}, {true,false,false,false} },
    //  Alg 8:  2->1->0(C), 3->1  [extra mod feeds middle of chain]
    { {1<<1, 1<<2|1<<3, 0, 0}, {true,false,false,false} },
    //  Alg 9:  3->1->0(C), 2->0(C)  [two separate chains into carrier]
    { {1<<1|1<<2, 1<<3, 0, 0}, {true,false,false,false} },
    //  Alg 10: 3->2->1->0(C), 2->0(C)  [chain with mid-point bypass to carrier]
    { {1<<1|1<<2, 1<<2, 1<<3, 0}, {true,false,false,false} },
    //  Alg 11: 2->1->0(C), 3->1  [Y: two mods fan into op1 which feeds carrier]
    { {1<<1, 1<<2|1<<3, 0, 0}, {true,false,false,false} },
    //  Alg 12: 2->1->0(C), 3(C)  [chain of 3 + bare fourth carrier]
    { {1<<1, 1<<2, 0, 0}, {true,false,false,true} },
    //  Alg 13: 2->1(C), 2->0(C), 3(C)  [one mod fans to two carriers + bare]
    { {1<<2, 1<<2, 0, 0}, {true,true,false,true} },
    //  Alg 14: 3->2->1(C), 3->0(C)  [chain exits mid-way + shortcut to op0]
    { {1<<3, 1<<2, 1<<3, 0}, {true,true,false,false} },
    //  Alg 15: 1->0(C), 3->2(C)  [two independent 2-op stacks]
    { {1<<1, 0, 1<<3, 0}, {true,false,true,false} },
    //  Alg 16: 1->0(C), 2(C), 3(C)  [one 2-op stack + two bare carriers]
    { {1<<1, 0, 0, 0}, {true,false,true,true} },
    //  Alg 17: 3->0(C), 2->1(C)  [two crossed 2-op stacks, different pairing]
    { {1<<3, 0, 1<<2, 0}, {true,true,false,false} },
    //  Alg 18: 3->0(C), 2->0(C), 1->0(C)  [star: all mods into single carrier]
    { {1<<1|1<<2|1<<3, 0, 0, 0}, {true,false,false,false} },
    //  Alg 19: 3->0(C), 2->0(C), 1(C)  [two mods into carrier + bare carrier]
    { {1<<2|1<<3, 0, 0, 0}, {true,true,false,false} },
    //  Alg 20: 3->2(C), 1->0(C)  [two independent 2-op stacks, ops swapped]
    { {1<<1, 0, 1<<3, 0}, {true,false,true,false} },
    //  Alg 21: 3->2(C), 3->1(C), 3->0(C)  [one mod fans to three carriers]
    { {1<<3, 1<<3, 1<<3, 0}, {true,true,true,false} },
    //  Alg 22: 3->2(C), 1(C), 0(C)  [one 2-op stack + two bare carriers]
    { {0, 0, 1<<3, 0}, {true,true,true,false} },
    //  Alg 23: 3->2(C), 3->1(C), 0(C)  [branching mod + independent bare carrier]
    { {0, 1<<3, 1<<3, 0}, {true,true,true,false} },
    //  Alg 24: 0(C), 1(C), 2(C), 3(C)  [all carriers — pure additive synthesis]
    { {0, 0, 0, 0}, {true,true,true,true} },
    //  Alg 25: 3->2->1(C), 0(C)  [chain of 3 exiting at op1 + bare op0 carrier]
    { {0, 1<<2, 1<<3, 0}, {true,true,false,false} },
    //  Alg 26: 3->1(C), 2->0(C)  [cross-paired 2-op stacks]
    { {1<<2, 1<<3, 0, 0}, {true,true,false,false} },
    //  Alg 27: 3->2->1(C), 2->0(C)  [chain forks: op2 also drives carrier op0]
    { {1<<2, 1<<2, 1<<3, 0}, {true,true,false,false} },
    //  Alg 28: 3->2->1->0(C), 3->1  [chain + shortcut to mid-point op1]
    { {1<<1, 1<<2, 1<<3|1<<2, 0}, {true,false,false,false} },
    //  Alg 29: 3->0(C), 2->1->0(C)  [direct mod + 2-deep chain both into carrier]
    { {1<<1|1<<3, 1<<2, 0, 0}, {true,false,false,false} },
    //  Alg 30: 3->1->0(C), 2->1  [extra mod feeds chain entry op1]
    { {1<<1, 1<<2|1<<3, 0, 0}, {true,false,false,false} },
    //  Alg 31: 3->2->0(C), 2->1->0(C)  [op2 shared: two paths converge on carrier]
    { {1<<1|1<<2, 1<<2, 1<<3, 0}, {true,false,false,false} },
    //  Alg 32: 3->0(C), 1->0(C), 2->1  [op2 feeds op1 feeds carrier, op3 direct too]
    { {1<<1|1<<3, 1<<2, 0, 0}, {true,false,false,false} },
};


// ============================================================
// FMParams — all cross-thread state via std::atomic
// ============================================================
struct FMParams {
    std::atomic<int>   algorithm   { 0 };   // 0..31

    struct Operator {
        std::atomic<float> ratio    { 1.0f };
        std::atomic<float> level    { 0.8f };
        std::atomic<float> feedback { 0.0f }; // 0..1 (only effective on self-fb capable ops)
        std::atomic<float> attack   { 0.001f };
        std::atomic<float> decay    { 0.3f };
        std::atomic<float> sustain  { 0.7f };
        std::atomic<float> release  { 0.3f };
        std::atomic<bool>  enabled  { true };
    };
    Operator ops[4];

    std::atomic<float> masterLevel  { 0.8f };

    // Master amp envelope
    std::atomic<float> ampAttack    { 0.001f };
    std::atomic<float> ampDecay     { 0.1f };
    std::atomic<float> ampSustain   { 1.0f };
    std::atomic<float> ampRelease   { 0.3f };

    // Master filter
    std::atomic<bool>  filterEnabled   { false };
    std::atomic<int>   filterType      { 0 };
    std::atomic<float> filterCutoff    { 8000.0f };
    std::atomic<float> filterResonance { 1.0f };

    // ── MPE mapping configuration (4.1) ─────────────────────────────────
    // mpePressureTarget: 0 = Amplitude, 1 = Filter Cutoff
    std::atomic<int>   mpePressureTarget { 0 };
    // mpeTimbreRange: multiplier range for Op1 ratio modulation via slide
    std::atomic<float> mpeTimbreRange    { 0.5f };
    // mpeBendRange: maximum pitch-bend range in semitones (default ±48)
    std::atomic<float> mpeBendRange      { 48.0f };
};

// ============================================================
// FMVoice — single voice, sample-accurate FM rendering
// ============================================================
class FMVoice {
public:
    void prepare(double sr) {
        sampleRate = sr;
        ampAdsr.setSampleRate(sr);
        for (auto& a : opAdsr) a.setSampleRate(sr);
        juce::dsp::ProcessSpec spec { sr, 8192, 2 };
        filter.prepare(spec);
        active = false;
    }

    void noteOn(int note, float vel, int mpeCh, const FMParams& p) {
        currentNote     = note;
        currentVelocity = vel;
        active          = true;
        feedbackSample  = 0.0f;
        mpeChannel            = mpeCh;
        mpePitchBendSemitones = 0.0f;
        mpePressure           = 1.0f;
        mpeTimbre             = 0.5f;
        for (auto& ph : opPhase) ph = 0.0;

        juce::ADSR::Parameters ampP { p.ampAttack, p.ampDecay, p.ampSustain, p.ampRelease };
        ampAdsr.setParameters(ampP);
        ampAdsr.noteOn();
        for (int i = 0; i < 4; ++i) {
            juce::ADSR::Parameters opP { p.ops[i].attack, p.ops[i].decay,
                                         p.ops[i].sustain, p.ops[i].release };
            opAdsr[i].setParameters(opP);
            opAdsr[i].noteOn();
        }
    }
    // Legacy overload without MPE channel
    void noteOn(int note, float vel, const FMParams& p) { noteOn(note, vel, 1, p); }

    void noteOff() {
        ampAdsr.noteOff();
        for (auto& a : opAdsr) a.noteOff();
    }

    bool isActive() const { return active; }
    int  getNote()  const { return currentNote; }
    int  getMpeChannel() const { return mpeChannel; }

    // MPE per-voice state — written from processBlock, read in renderNextBlock.
    // All on the render thread, no atomics needed.
    int   mpeChannel            {1};
    float mpePitchBendSemitones {0.0f};
    float mpePressure           {1.0f};
    float mpeTimbre             {0.5f};

    void renderNextBlock(juce::AudioBuffer<float>& out, int startSample, int numSamples,
                         const FMParams& p) {
        if (!active) return;

        const int algoIdx   = juce::jlimit(0, 31, p.algorithm.load());
        const auto& algo    = kDX7Algorithms[algoIdx];
        // MPE pitch bend applied to baseFreq
        const float bendRatio = std::pow(2.0f, mpePitchBendSemitones / 12.0f);
        const float baseFreq = (float)juce::MidiMessage::getMidiNoteInHertz(currentNote) * bendRatio;
        const float sr       = (float)sampleRate;

        // Cache atomic params once per block
        float opRatio[4], opLevel[4], opFB[4];
        bool  opEnabled[4];
        for (int i = 0; i < 4; ++i) {
            opRatio[i]   = p.ops[i].ratio.load();
            opLevel[i]   = p.ops[i].level.load();
            opFB[i]      = p.ops[i].feedback.load() * juce::MathConstants<float>::pi; // 0..π
            opEnabled[i] = p.ops[i].enabled.load();
        }
        const float masterLvl = p.masterLevel.load();
        const bool  filterOn  = p.filterEnabled.load();

        // Update ADSR params block-wise
        {
            juce::ADSR::Parameters ampP { p.ampAttack.load(), p.ampDecay.load(),
                                          p.ampSustain.load(), p.ampRelease.load() };
            ampAdsr.setParameters(ampP);
        }
        for (int i = 0; i < 4; ++i) {
            juce::ADSR::Parameters opP { p.ops[i].attack.load(), p.ops[i].decay.load(),
                                         p.ops[i].sustain.load(), p.ops[i].release.load() };
            opAdsr[i].setParameters(opP);
        }

        if (filterOn) {
            filter.setType(p.filterType.load() == 0
                ? juce::dsp::StateVariableTPTFilterType::lowpass
                : p.filterType.load() == 1
                    ? juce::dsp::StateVariableTPTFilterType::highpass
                    : juce::dsp::StateVariableTPTFilterType::bandpass);
            filter.setCutoffFrequency(juce::jlimit(20.0f, 20000.0f, p.filterCutoff.load()));
            filter.setResonance(juce::jlimit(0.1f, 10.0f, p.filterResonance.load()));
        }

        for (int s = 0; s < numSamples; ++s) {
            const float ampEnv = ampAdsr.getNextSample();
            if (!ampAdsr.isActive()) { active = false; break; }

            float opEnv[4], opSin[4];
            for (int op = 0; op < 4; ++op)
                opEnv[op] = opEnabled[op] ? opAdsr[op].getNextSample() : 0.0f;

            // --- Two-pass FM computation ---
            // Pass 1: compute raw oscillator output for every op (no FM applied yet)
            // This gives us modulator outputs for use in Pass 2.
            for (int op = 0; op < 4; ++op) {
                float ph = (float)opPhase[op];
                // Self-feedback on op 0 (carrier) only
                float fbPhaseOffset = (op == 0) ? feedbackSample * opFB[op] : 0.0f;
                opSin[op] = std::sin(juce::MathConstants<float>::twoPi * ph + fbPhaseOffset);
                opSin[op] *= opEnv[op] * opLevel[op];
            }

            // MPE timbre: ratio offset applied to all carrier operators.
            // Range: (mpeTimbre-0.5) * mpeTimbreRange  →  e.g. ±0.25 at range=0.5
            const float timbreRatioMod = (mpeTimbre - 0.5f) * p.mpeTimbreRange.load();

            // Pass 2: carriers re-evaluate with FM modulation from modulators
            float audioOut = 0.0f;
            for (int op = 0; op < 4; ++op) {
                // Apply timbre offset to carrier op ratios; clamp so freq > 0.
                float ratio = opRatio[op];
                if (algo.isCarrier[op])
                    ratio = juce::jlimit(0.01f, 32.0f, ratio + timbreRatioMod);
                float freq = baseFreq * ratio;
                float ph   = (float)opPhase[op];

                // Accumulate FM modulation from all source operators
                float fmMod = 0.0f;
                for (int src = 0; src < 4; ++src) {
                    if ((algo.modulatedBy[op] & (1 << src)) && src != op)
                        fmMod += opSin[src];
                }
                // Self-feedback for op 0
                float fbMod = (op == 0) ? feedbackSample * opFB[op] : 0.0f;

                float sample = std::sin(juce::MathConstants<float>::twoPi * ph
                                        + fmMod * 4.0f   // modulation index scale
                                        + fbMod);
                sample *= opEnv[op] * opLevel[op];

                // Advance phase
                opPhase[op] = std::fmod(opPhase[op] + freq / sampleRate, 1.0);

                if (algo.isCarrier[op])
                    audioOut += sample;
            }

            // Update feedback state from carrier output
            feedbackSample = audioOut;

            // MPE amplitude: pressure scales output if pressureTarget == 0
            const float ampMpe = (p.mpePressureTarget.load() == 0) ? mpePressure : 1.0f;
            audioOut *= ampEnv * currentVelocity * masterLvl * ampMpe;

            if (filterOn)
                audioOut = filter.processSample(0, audioOut);

            out.addSample(0, startSample + s, audioOut);
            out.addSample(1, startSample + s, audioOut);
        }
    }

private:
    double   sampleRate     { 44100.0 };
    bool     active         { false };
    int      currentNote    { 0 };
    float    currentVelocity{ 0.0f };
    float    feedbackSample { 0.0f };

    double   opPhase[4]     { 0.0, 0.0, 0.0, 0.0 };

    juce::ADSR ampAdsr;
    juce::ADSR opAdsr[4];
    juce::dsp::StateVariableTPTFilter<float> filter;
};

// ============================================================
// FMSynthProcessor — 8-voice polyphonic FM synthesizer
// ============================================================
class FMSynthProcessor : public InstrumentProcessor {
public:
    FMParams params;

    FMSynthProcessor() = default;

    void prepareToPlay(double sampleRate) override {
        for (auto& v : voices) v.prepare(sampleRate);
    }

    void processBlock(juce::AudioBuffer<float>& outBuffer,
                      const juce::MidiBuffer&   midiMessages) override {
        int numSamples = outBuffer.getNumSamples();
        int pos        = 0;

        for (const auto meta : midiMessages) {
            auto msg = meta.getMessage();
            int sp   = meta.samplePosition;
            if (sp > pos) { renderVoices(outBuffer, pos, sp - pos); pos = sp; }

            if      (msg.isNoteOn())       handleNoteOn (msg.getNoteNumber(), msg.getChannel(), msg.getFloatVelocity());
            else if (msg.isNoteOff())      handleNoteOff(msg.getNoteNumber());
            else if (msg.isAllNotesOff())  { for (auto& v : voices) v.noteOff(); }
            // ── 4.1 MPE expression handlers ─────────────────────────────────
            else if (msg.isPitchWheel()) {
                float semitones = ((msg.getPitchWheelValue() - 8192) / 8192.0f)
                                  * params.mpeBendRange.load();
                for (auto& v : voices)
                    if (v.isActive() && v.getMpeChannel() == msg.getChannel())
                        v.mpePitchBendSemitones = semitones;
            }
            else if (msg.isChannelPressure()) {
                float p = msg.getChannelPressureValue() / 127.0f;
                for (auto& v : voices)
                    if (v.isActive() && v.getMpeChannel() == msg.getChannel())
                        v.mpePressure = p;
            }
            else if (msg.isController() && msg.getControllerNumber() == 74) {
                float t = msg.getControllerValue() / 127.0f;
                for (auto& v : voices)
                    if (v.isActive() && v.getMpeChannel() == msg.getChannel())
                        v.mpeTimbre = t;
            }
        }
        if (pos < numSamples) renderVoices(outBuffer, pos, numSamples - pos);
    }

    void clear() override { for (auto& v : voices) v.noteOff(); }

    juce::ValueTree saveState() const override {
        juce::ValueTree tree("FMSynthState");
        tree.setProperty("algorithm",     params.algorithm.load(),      nullptr);
        tree.setProperty("masterLevel",   params.masterLevel.load(),    nullptr);
        tree.setProperty("ampAttack",     params.ampAttack.load(),      nullptr);
        tree.setProperty("ampDecay",      params.ampDecay.load(),       nullptr);
        tree.setProperty("ampSustain",    params.ampSustain.load(),     nullptr);
        tree.setProperty("ampRelease",    params.ampRelease.load(),     nullptr);
        tree.setProperty("filterEnabled", params.filterEnabled.load(),  nullptr);
        tree.setProperty("filterType",    params.filterType.load(),     nullptr);
        tree.setProperty("filterCutoff",  params.filterCutoff.load(),   nullptr);
        tree.setProperty("filterRes",     params.filterResonance.load(),nullptr);
        // MPE config (4.1)
        tree.setProperty("mpePT",   params.mpePressureTarget.load(), nullptr);
        tree.setProperty("mpeTR",   params.mpeTimbreRange.load(),    nullptr);
        tree.setProperty("mpeBR",   params.mpeBendRange.load(),      nullptr);
        for (int i = 0; i < 4; ++i) {
            juce::ValueTree opN("Op" + juce::String(i));
            opN.setProperty("ratio",    params.ops[i].ratio.load(),    nullptr);
            opN.setProperty("level",    params.ops[i].level.load(),    nullptr);
            opN.setProperty("feedback", params.ops[i].feedback.load(), nullptr);
            opN.setProperty("attack",   params.ops[i].attack.load(),   nullptr);
            opN.setProperty("decay",    params.ops[i].decay.load(),    nullptr);
            opN.setProperty("sustain",  params.ops[i].sustain.load(),  nullptr);
            opN.setProperty("release",  params.ops[i].release.load(),  nullptr);
            opN.setProperty("enabled",  params.ops[i].enabled.load(),  nullptr);
            tree.addChild(opN, -1, nullptr);
        }
        return tree;
    }

    void loadState(const juce::ValueTree& tree) override {
        params.algorithm.store    ((int)  tree.getProperty("algorithm",     0));
        params.masterLevel.store  ((float)tree.getProperty("masterLevel",   0.8f));
        params.ampAttack.store    ((float)tree.getProperty("ampAttack",     0.001f));
        params.ampDecay.store     ((float)tree.getProperty("ampDecay",      0.1f));
        params.ampSustain.store   ((float)tree.getProperty("ampSustain",    1.0f));
        params.ampRelease.store   ((float)tree.getProperty("ampRelease",    0.3f));
        params.filterEnabled.store((bool) tree.getProperty("filterEnabled", false));
        params.filterType.store   ((int)  tree.getProperty("filterType",    0));
        params.filterCutoff.store ((float)tree.getProperty("filterCutoff",  8000.0f));
        params.filterResonance.store((float)tree.getProperty("filterRes",   1.0f));
        // MPE config (4.1)
        params.mpePressureTarget.store((int)  tree.getProperty("mpePT", 0));
        params.mpeTimbreRange.store   ((float)tree.getProperty("mpeTR", 0.5f));
        params.mpeBendRange.store     ((float)tree.getProperty("mpeBR", 48.0f));
        for (int i = 0; i < 4; ++i) {
            auto n = tree.getChildWithName("Op" + juce::String(i));
            if (n.isValid()) {
                params.ops[i].ratio.store   ((float)n.getProperty("ratio",    1.0f));
                params.ops[i].level.store   ((float)n.getProperty("level",    0.8f));
                params.ops[i].feedback.store((float)n.getProperty("feedback", 0.0f));
                params.ops[i].attack.store  ((float)n.getProperty("attack",   0.001f));
                params.ops[i].decay.store   ((float)n.getProperty("decay",    0.3f));
                params.ops[i].sustain.store ((float)n.getProperty("sustain",  0.7f));
                params.ops[i].release.store ((float)n.getProperty("release",  0.3f));
                params.ops[i].enabled.store ((bool) n.getProperty("enabled",  true));
            }
        }
    }

    void registerAutomationParameters(AutomationRegistry* registry) override {
        if (!registry) return;
        
        // IDs must exactly match the `parameterId` property set in FMSynthComponent.h
        registry->registerParameter("FM/Global/Master Level", &params.masterLevel, 0.0f, 2.0f);
        
        registry->registerParameter("FM/Global Amp/Attack", &params.ampAttack, 0.001f, 5.0f);
        registry->registerParameter("FM/Global Amp/Decay", &params.ampDecay, 0.001f, 5.0f);
        registry->registerParameter("FM/Global Amp/Sustain", &params.ampSustain, 0.0f, 1.0f);
        registry->registerParameter("FM/Global Amp/Release", &params.ampRelease, 0.001f, 5.0f);
        
        registry->registerParameter("FM/Filter/Cutoff", &params.filterCutoff, 20.0f, 20000.0f);
        registry->registerParameter("FM/Filter/Resonance", &params.filterResonance, 0.1f, 10.0f);
        
        for (int i = 0; i < 4; ++i) {
            juce::String prefix = "FM/OP " + juce::String(i + 1) + "/";
            registry->registerParameter(prefix + "Ratio", &params.ops[i].ratio, 0.125f, 16.0f);
            registry->registerParameter(prefix + "Level", &params.ops[i].level, 0.0f, 1.0f);
            registry->registerParameter(prefix + "Feedback", &params.ops[i].feedback, 0.0f, 1.0f);
            registry->registerParameter(prefix + "Attack", &params.ops[i].attack, 0.001f, 5.0f);
            registry->registerParameter(prefix + "Decay", &params.ops[i].decay, 0.001f, 5.0f);
            registry->registerParameter(prefix + "Sustain", &params.ops[i].sustain, 0.0f, 1.0f);
            registry->registerParameter(prefix + "Release", &params.ops[i].release, 0.001f, 5.0f);
        }
    }

    std::unique_ptr<juce::Component> createEditor() override;
    juce::String getName() const override { return "FMSynth"; }

private:
    void handleNoteOn(int note, int ch, float vel) {
        for (auto& v : voices) {
            if (!v.isActive()) { v.noteOn(note, vel, ch, params); return; }
        }
        for (auto& v : voices) {
            if (v.getNote() == note) { v.noteOn(note, vel, ch, params); return; }
        }
        voices[0].noteOn(note, vel, ch, params);
    }
    // Legacy overload without channel
    void handleNoteOn(int note, float vel) { handleNoteOn(note, 1, vel); }
    void handleNoteOff(int note) {
        for (auto& v : voices)
            if (v.isActive() && v.getNote() == note) v.noteOff();
    }
    void renderVoices(juce::AudioBuffer<float>& buf, int start, int num) {
        for (auto& v : voices)
            if (v.isActive()) v.renderNextBlock(buf, start, num, params);
    }

    std::array<FMVoice, 8> voices;
};
