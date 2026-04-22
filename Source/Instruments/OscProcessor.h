#pragma once
#include <JuceHeader.h>
#include <atomic>
#include <array>
#include <cmath>
#include "LockFreeQueue.h"

struct OscCommand {
    enum Type { NoteOn, NoteOff, AllNotesOff };
    Type type;
    int noteNumber;
    float velocity;
};

struct OscParams {
    std::atomic<float> masterLevel { 0.8f };
    
    struct Osc {
        std::atomic<bool> enabled { true };
        std::atomic<int> waveform { 2 }; // 0:Sine, 1:Tri, 2:Saw, 3:Square, 4:WT
        std::atomic<float> octave { 0.0f };
        std::atomic<float> coarse { 0.0f };
        std::atomic<float> fine { 0.0f };
        std::atomic<float> level { 0.8f };
        std::atomic<float> pan { 0.5f };
        std::atomic<int> unisonVoices { 1 };
        std::atomic<float> unisonDetune { 0.1f };
        std::atomic<float> unisonSpread { 0.5f };
        std::atomic<float> wtPos { 0.0f };
    };
    Osc oscA, oscB, oscC;
    
    std::atomic<float> fmBtoA { 0.0f };
    std::atomic<float> fmCtoA { 0.0f };
    
    struct Sub {
        std::atomic<bool> enabled { false };
        std::atomic<int> waveform { 0 }; // 0:Sine, 1:Square
        std::atomic<float> level { 0.5f };
        std::atomic<bool> directOut { false };
    } sub;

    struct Noise {
        std::atomic<bool> enabled { false };
        std::atomic<float> level { 0.0f };
        std::atomic<int> type { 0 }; // 0:White
    } noise;
    
    struct ADSRNode {
        std::atomic<float> attack { 0.01f };
        std::atomic<float> decay { 0.1f };
        std::atomic<float> sustain { 1.0f };
        std::atomic<float> release { 0.1f };
    };
    ADSRNode ampEnv, filterEnv;

    struct Filter {
        std::atomic<bool> enabled { false };
        std::atomic<int> type { 0 }; // 0:LP12, 1:HP12, 2:BP12
        std::atomic<float> cutoff { 2000.0f };
        std::atomic<float> resonance { 1.0f };
        std::atomic<float> envAmount { 0.0f };
        std::atomic<float> keytrack { 0.0f };
    } filter;
};

class OscVoice {
public:
    void prepare(double sr) {
        sampleRate = sr;
        ampAdsr.setSampleRate(sr);
        filterAdsr.setSampleRate(sr);
        juce::dsp::ProcessSpec spec;
        spec.sampleRate = sr;
        spec.maximumBlockSize = 8192;
        spec.numChannels = 2;
        svf.prepare(spec);
    }
    
    void noteOn(int note, float velocity, const OscParams& params) {
        currentNote = note;
        currentVelocity = velocity;
        active = true;
        
        juce::ADSR::Parameters ampParams { params.ampEnv.attack, params.ampEnv.decay, params.ampEnv.sustain, params.ampEnv.release };
        ampAdsr.setParameters(ampParams);
        ampAdsr.noteOn();
        
        juce::ADSR::Parameters filterParams { params.filterEnv.attack, params.filterEnv.decay, params.filterEnv.sustain, params.filterEnv.release };
        filterAdsr.setParameters(filterParams);
        filterAdsr.noteOn();
        
        for (int i=0; i<3; ++i) {
            for (int v=0; v<8; ++v) phase[i][v] = 0.0;
        }
        subPhase = 0.0;
    }
    
    void noteOff() {
        ampAdsr.noteOff();
        filterAdsr.noteOff();
    }
    
    bool isActive() const { return active; }
    int getNote() const { return currentNote; }
    
    void renderNextBlock(juce::AudioBuffer<float>& outputBuffer, int startSample, int numSamples, const OscParams& p) {
        if (!active) return;
        
        juce::ADSR::Parameters ampParams { p.ampEnv.attack, p.ampEnv.decay, p.ampEnv.sustain, p.ampEnv.release };
        ampAdsr.setParameters(ampParams);
        juce::ADSR::Parameters filterParams { p.filterEnv.attack, p.filterEnv.decay, p.filterEnv.sustain, p.filterEnv.release };
        filterAdsr.setParameters(filterParams);
        
        float baseFreq = std::max(20.0f, (float)juce::MidiMessage::getMidiNoteInHertz(currentNote));
        
        float fmBtoA = p.fmBtoA.load();
        float fmCtoA = p.fmCtoA.load();
        
        svf.setType(p.filter.type == 0 ? juce::dsp::StateVariableTPTFilterType::lowpass :
                    p.filter.type == 1 ? juce::dsp::StateVariableTPTFilterType::highpass :
                                         juce::dsp::StateVariableTPTFilterType::bandpass);
        svf.setResonance(juce::jlimit(0.1f, 10.0f, p.filter.resonance.load()));

        for (int i = 0; i < numSamples; ++i) {
            float envAmp = ampAdsr.getNextSample();
            float envFilt = filterAdsr.getNextSample();
            if (!ampAdsr.isActive()) {
                active = false;
                break;
            }
            
            auto evalOsc = [&](const OscParams::Osc& osc, int oscIdx, float fmMod) {
                if (!osc.enabled) return std::pair<float, float>(0.0f, 0.0f);
                float outL = 0.0f, outR = 0.0f;
                float freqMult = std::pow(2.0f, osc.octave.load() + osc.coarse.load() / 12.0f + osc.fine.load() / 1200.0f);
                float freq = baseFreq * freqMult;
                int voices = osc.unisonVoices.load();
                float detuneMax = osc.unisonDetune.load() * 0.05f * freq; // max detune in Hz
                float spread = osc.unisonSpread.load();
                int waveType = osc.waveform.load();
                
                for (int v = 0; v < voices; ++v) {
                    float detune = voices == 1 ? 0.0f : detuneMax * ((float)v / (voices - 1) * 2.0f - 1.0f);
                    float curFreq = freq + detune + fmMod;
                    curFreq = std::clamp(curFreq, 0.0f, 20000.0f);
                    
                    float dp = curFreq / sampleRate;
                    float p = phase[oscIdx][v];
                    float sample = 0.0f;
                    
                    if (waveType == 0) sample = std::sin(juce::MathConstants<float>::twoPi * p); // sine
                    else if (waveType == 1) sample = 2.0f * std::abs(2.0f * (p - std::floor(p + 0.5f))) - 1.0f; // tri
                    else if (waveType == 2) sample = 2.0f * (p - std::floor(p + 0.5f)); // saw
                    else if (waveType == 3) sample = (p < 0.5f) ? 1.0f : -1.0f; // square
                    else sample = std::sin(juce::MathConstants<float>::twoPi * p); // WT placeholder

                    phase[oscIdx][v] = std::fmod(p + dp, 1.0f);
                    
                    float panV = voices == 1 ? 0.5f : ((float)v / (voices - 1));
                    panV = 0.5f + (panV - 0.5f) * spread; // Apply spread
                    float panMain = osc.pan.load(); // 0 to 1
                    panV = std::clamp(panV + (panMain - 0.5f), 0.0f, 1.0f);
                    
                    float lGain = std::cos(panV * juce::MathConstants<float>::halfPi);
                    float rGain = std::sin(panV * juce::MathConstants<float>::halfPi);
                    
                    outL += sample * lGain;
                    outR += sample * rGain;
                }
                outL *= osc.level.load() / voices;
                outR *= osc.level.load() / voices;
                return std::make_pair(outL, outR);
            };

            auto resC = evalOsc(p.oscC, 2, 0.0f);
            auto resB = evalOsc(p.oscB, 1, 0.0f);
            
            float fmModToA = 0.0f;
            if (fmBtoA > 0.0f) fmModToA += (resB.first + resB.second) * fmBtoA * 1000.0f;
            if (fmCtoA > 0.0f) fmModToA += (resC.first + resC.second) * fmCtoA * 1000.0f;
            
            auto resA = evalOsc(p.oscA, 0, fmModToA);
            
            float subOut = 0.0f;
            if (p.sub.enabled) {
                float subFreq = baseFreq * 0.5f;
                float dp = subFreq / sampleRate;
                if (p.sub.waveform == 0) subOut = std::sin(juce::MathConstants<float>::twoPi * subPhase);
                else subOut = (subPhase < 0.5f) ? 1.0f : -1.0f;
                subPhase = std::fmod(subPhase + dp, 1.0f);
                subOut *= p.sub.level;
            }
            
            float noiseOut = 0.0f;
            if (p.noise.enabled) {
                noiseOut = (random.nextFloat() * 2.0f - 1.0f) * p.noise.level;
            }
            
            float mixedL = resA.first + resB.first + resC.first + noiseOut;
            float mixedR = resA.second + resB.second + resC.second + noiseOut;
            
            if (!p.sub.directOut) {
                mixedL += subOut;
                mixedR += subOut;
            }

            if (p.filter.enabled) {
                float baseCutoff = p.filter.cutoff;
                float keytrack = p.filter.keytrack;
                float envAmt = p.filter.envAmount;
                
                float ktMult = std::pow(2.0f, ((currentNote - 60) / 12.0f) * keytrack);
                float envMult = std::pow(2.0f, envAmt * 4.0f * envFilt); // max 4 octaves env mod
                
                float cutoff = std::clamp(baseCutoff * ktMult * envMult, 20.0f, 20000.0f);
                svf.setCutoffFrequency(cutoff);
                
                mixedL = svf.processSample(0, mixedL);
                mixedR = svf.processSample(1, mixedR);
            }
            
            if (p.sub.directOut) {
                mixedL += subOut;
                mixedR += subOut;
            }
            
            mixedL *= envAmp * currentVelocity * p.masterLevel;
            mixedR *= envAmp * currentVelocity * p.masterLevel;
            
            outputBuffer.addSample(0, startSample + i, mixedL);
            outputBuffer.addSample(1, startSample + i, mixedR);
        }
    }
    
private:
    double sampleRate { 44100.0 };
    bool active { false };
    int currentNote { 0 };
    float currentVelocity { 0.0f };
    
    juce::ADSR ampAdsr, filterAdsr;
    juce::dsp::StateVariableTPTFilter<float> svf;
    juce::Random random;
    
    float phase[3][8] = {0.0f};
    float subPhase = 0.0f;
};

#include "Instrument.h"

class OscProcessor : public InstrumentProcessor {
public:
    OscParams params;

    OscProcessor() {
        for (auto& v : voices) v = std::make_unique<OscVoice>();
    }

    void prepareToPlay(double sampleRate) override {
        for (auto& v : voices) v->prepare(sampleRate);
    }
    
    void processBlock(juce::AudioBuffer<float>& outBuffer, const juce::MidiBuffer& midiMessages) override {
        int numSamples = outBuffer.getNumSamples();
        int currentPos = 0;
        for (const auto meta : midiMessages) {
            auto msg = meta.getMessage();
            int samplePos = meta.samplePosition;

            if (samplePos > currentPos) {
                renderVoices(outBuffer, currentPos, samplePos - currentPos);
                currentPos = samplePos;
            }

            if (msg.isNoteOn()) {
                handleNoteOn(msg.getNoteNumber(), msg.getFloatVelocity());
            } else if (msg.isNoteOff()) {
                handleNoteOff(msg.getNoteNumber());
            } else if (msg.isAllNotesOff()) {
                for (auto& v : voices) v->noteOff();
            }
        }
        
        if (currentPos < numSamples) {
            renderVoices(outBuffer, currentPos, numSamples - currentPos);
        }
    }
    
    void clear() override {
        for (auto& v : voices) v->noteOff();
    }
    
    juce::ValueTree saveState() const override {
        juce::ValueTree tree("OscillatorState");
        
        auto saveOsc = [&](const juce::String& name, const OscParams::Osc& o) {
            juce::ValueTree node(name);
            node.setProperty("enabled", o.enabled.load(), nullptr);
            node.setProperty("waveform", o.waveform.load(), nullptr);
            node.setProperty("octave", o.octave.load(), nullptr);
            node.setProperty("coarse", o.coarse.load(), nullptr);
            node.setProperty("level", o.level.load(), nullptr);
            node.setProperty("unisonVoices", o.unisonVoices.load(), nullptr);
            node.setProperty("unisonDetune", o.unisonDetune.load(), nullptr);
            tree.addChild(node, -1, nullptr);
        };
        
        saveOsc("OscA", params.oscA);
        saveOsc("OscB", params.oscB);
        saveOsc("OscC", params.oscC);
        
        juce::ValueTree filterNode("Filter");
        filterNode.setProperty("enabled", params.filter.enabled.load(), nullptr);
        filterNode.setProperty("type", params.filter.type.load(), nullptr);
        filterNode.setProperty("cutoff", params.filter.cutoff.load(), nullptr);
        filterNode.setProperty("resonance", params.filter.resonance.load(), nullptr);
        filterNode.setProperty("envAmount", params.filter.envAmount.load(), nullptr);
        tree.addChild(filterNode, -1, nullptr);
        
        return tree;
    }

    void loadState(const juce::ValueTree& tree) override {
        auto loadOsc = [&](const juce::String& name, OscParams::Osc& o) {
            auto node = tree.getChildWithName(name);
            if (node.isValid()) {
                o.enabled.store(node.getProperty("enabled", o.enabled.load()));
                o.waveform.store(node.getProperty("waveform", o.waveform.load()));
                o.octave.store(node.getProperty("octave", o.octave.load()));
                o.coarse.store(node.getProperty("coarse", o.coarse.load()));
                o.level.store(node.getProperty("level", o.level.load()));
                o.unisonVoices.store(node.getProperty("unisonVoices", o.unisonVoices.load()));
                o.unisonDetune.store(node.getProperty("unisonDetune", o.unisonDetune.load()));
            }
        };
        
        loadOsc("OscA", params.oscA);
        loadOsc("OscB", params.oscB);
        loadOsc("OscC", params.oscC);
        
        auto filterNode = tree.getChildWithName("Filter");
        if (filterNode.isValid()) {
            params.filter.enabled.store(filterNode.getProperty("enabled", params.filter.enabled.load()));
            params.filter.type.store(filterNode.getProperty("type", params.filter.type.load()));
            params.filter.cutoff.store(filterNode.getProperty("cutoff", params.filter.cutoff.load()));
            params.filter.resonance.store(filterNode.getProperty("resonance", params.filter.resonance.load()));
            params.filter.envAmount.store(filterNode.getProperty("envAmount", params.filter.envAmount.load()));
        }
    }
    
    std::unique_ptr<juce::Component> createEditor() override;
    juce::String getName() const override { return "Oscillator"; }

    void moveFrom(OscProcessor& other) {
        // Simple state swap (if needed by DAW track management)
    }

private:
    void handleNoteOn(int note, float vel) {
        for (auto& v : voices) {
            if (!v->isActive()) {
                v->noteOn(note, vel, params);
                return;
            }
        }
        // Voice stealing
        voices[0]->noteOn(note, vel, params);
    }
    
    void handleNoteOff(int note) {
        for (auto& v : voices) {
            if (v->isActive() && v->getNote() == note) {
                v->noteOff();
            }
        }
    }
    
    void renderVoices(juce::AudioBuffer<float>& buffer, int start, int num) {
        for (auto& v : voices) {
            if (v->isActive()) {
                v->renderNextBlock(buffer, start, num, params);
            }
        }
    }

    std::array<std::unique_ptr<OscVoice>, 8> voices;
};
