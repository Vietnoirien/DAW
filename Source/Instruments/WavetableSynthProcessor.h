#pragma once
#include <JuceHeader.h>
#include <atomic>
#include <array>
#include <cmath>
#include "Instrument.h"

// ============================================================
// Procedural wavetable bank: 8 waveforms x 2048 samples
// ============================================================
static constexpr int kWTSize  = 2048;
static constexpr int kWTCount = 8;

class WavetableBank {
public:
    WavetableBank() { generate(); }

    float getMorphSample(float wtPos, float samplePhase) const {
        // wtPos: 0..(kWTCount-1), samplePhase: 0..1
        int   wt0  = juce::jlimit(0, kWTCount - 1, (int)wtPos);
        float frac = wtPos - (int)wtPos;
        int   wt1  = juce::jmin(wt0 + 1, kWTCount - 1);
        float idx  = samplePhase * kWTSize;
        int   i0   = (int)idx & (kWTSize - 1);
        int   i1   = (i0 + 1) & (kWTSize - 1);
        float tf   = idx - (int)idx;
        float s0   = tables[wt0][i0] + tf * (tables[wt0][i1] - tables[wt0][i0]);
        float s1   = tables[wt1][i0] + tf * (tables[wt1][i1] - tables[wt1][i0]);
        return s0 + frac * (s1 - s0);
    }

private:
    void generate() {
        const float tp = juce::MathConstants<float>::twoPi;
        // 0: Sine
        for (int i=0;i<kWTSize;++i) tables[0][i]=std::sin(tp*i/kWTSize);
        // 1: Triangle
        for (int i=0;i<kWTSize;++i){float t=i/(float)kWTSize; tables[1][i]=2.0f*std::abs(2.0f*(t-std::floor(t+0.5f)))-1.0f;}
        // 2: Saw
        for (int i=0;i<kWTSize;++i) tables[2][i]=2.0f*(i/(float)kWTSize)-1.0f;
        // 3: Reverse Saw
        for (int i=0;i<kWTSize;++i) tables[3][i]=1.0f-2.0f*(i/(float)kWTSize);
        // 4: Square 50%
        for (int i=0;i<kWTSize;++i) tables[4][i]=(i<kWTSize/2)?1.0f:-1.0f;
        // 5: Pulse 25%
        for (int i=0;i<kWTSize;++i) tables[5][i]=(i<kWTSize/4)?1.0f:-1.0f;
        // 6: Super-saw (3 detuned saws)
        for (int i=0;i<kWTSize;++i){
            float t=i/(float)kWTSize;
            tables[6][i]=(2.0f*t-1.0f + (2.0f*std::fmod(t+0.013f,1.0f)-1.0f)*0.7f
                         +(2.0f*std::fmod(t+0.027f,1.0f)-1.0f)*0.5f)/2.2f;
        }
        // 7: Additive (6 harmonics)
        for (int i=0;i<kWTSize;++i){
            float t=tp*i/kWTSize; float v=0.0f;
            for(int h=1;h<=6;++h) v+=std::sin(h*t)/h;
            tables[7][i]=v*0.5f;
        }
    }
    float tables[kWTCount][kWTSize];
};
static const WavetableBank kGlobalWT;

// ============================================================
// WTParams — all atomics
// ============================================================
struct WTParams {
    struct Osc {
        std::atomic<bool>  enabled{true};
        std::atomic<float> wtPos{0.0f};
        std::atomic<float> octave{0.0f};
        std::atomic<float> coarse{0.0f};
        std::atomic<float> fine{0.0f};
        std::atomic<float> level{0.8f};
        std::atomic<float> pan{0.5f};
        std::atomic<int>   unisonVoices{1};
        std::atomic<float> unisonDetune{0.1f};
        std::atomic<float> unisonSpread{0.5f};
    };
    Osc oscA, oscB;

    std::atomic<bool>  subEnabled{false};   std::atomic<float> subLevel{0.4f};
    std::atomic<bool>  noiseEnabled{false}; std::atomic<float> noiseLevel{0.0f};
    std::atomic<float> masterLevel{0.8f};

    std::atomic<bool>  filterEnabled{true};
    std::atomic<int>   filterType{0};
    std::atomic<float> filterCutoff{8000.0f};
    std::atomic<float> filterRes{0.707f};
    std::atomic<float> filterEnvAmt{0.0f};
    std::atomic<float> filterKeytrack{0.0f};

    std::atomic<float> ampA{0.001f}, ampD{0.1f}, ampS{1.0f}, ampR{0.2f};
    std::atomic<float> filtA{0.01f}, filtD{0.2f}, filtS{0.5f}, filtR{0.3f};
};

// ============================================================
// WTVoice
// ============================================================
class WTVoice {
public:
    void prepare(double sr) {
        sampleRate = sr;
        ampAdsr.setSampleRate(sr); filtAdsr.setSampleRate(sr);
        juce::dsp::ProcessSpec spec{sr, 8192, 2};
        filter.prepare(spec);
        active = false;
    }
    void noteOn(int note, float vel, const WTParams& p) {
        currentNote=note; currentVelocity=vel; active=true;
        for(auto& ph:phaseA) ph=0.0f;
        for(auto& ph:phaseB) ph=0.0f;
        subPhase=0.0f;
        juce::ADSR::Parameters ap{p.ampA,p.ampD,p.ampS,p.ampR};
        ampAdsr.setParameters(ap); ampAdsr.noteOn();
        juce::ADSR::Parameters fp{p.filtA,p.filtD,p.filtS,p.filtR};
        filtAdsr.setParameters(fp); filtAdsr.noteOn();
    }
    void noteOff() { ampAdsr.noteOff(); filtAdsr.noteOff(); }
    bool isActive() const { return active; }
    int  getNote()  const { return currentNote; }

    void renderNextBlock(juce::AudioBuffer<float>& out, int startSample, int numSamples, const WTParams& p) {
        if (!active) return;
        const float sr = (float)sampleRate;
        const float bf = (float)juce::MidiMessage::getMidiNoteInHertz(currentNote);
        {
            juce::ADSR::Parameters ap{p.ampA,p.ampD,p.ampS,p.ampR}; ampAdsr.setParameters(ap);
            juce::ADSR::Parameters fp{p.filtA,p.filtD,p.filtS,p.filtR}; filtAdsr.setParameters(fp);
        }
        const float wtA=p.oscA.wtPos.load(), wtB=p.oscB.wtPos.load();
        const bool aEn=p.oscA.enabled.load(), bEn=p.oscB.enabled.load();
        const float aLvl=p.oscA.level.load(), bLvl=p.oscB.level.load();
        const float aPan=p.oscA.pan.load(),   bPan=p.oscB.pan.load();
        const float ml=p.masterLevel.load();
        const bool fOn=p.filterEnabled.load();
        const float fEnv=p.filterEnvAmt.load(), fKt=p.filterKeytrack.load();
        auto freq=[&](const WTParams::Osc& o){ return bf*std::pow(2.0f,o.octave.load()+o.coarse.load()/12.0f+o.fine.load()/1200.0f); };
        const float fA=freq(p.oscA), fB=freq(p.oscB);
        const int uA=p.oscA.unisonVoices.load(), uB=p.oscB.unisonVoices.load();
        const float dA=p.oscA.unisonDetune.load()*0.05f*fA, dB=p.oscB.unisonDetune.load()*0.05f*fB;
        const float sA=p.oscA.unisonSpread.load(), sB=p.oscB.unisonSpread.load();
        if(fOn){
            filter.setType(p.filterType.load()==0?juce::dsp::StateVariableTPTFilterType::lowpass:
                           p.filterType.load()==1?juce::dsp::StateVariableTPTFilterType::highpass:
                                                   juce::dsp::StateVariableTPTFilterType::bandpass);
            filter.setResonance(juce::jlimit(0.1f,10.0f,p.filterRes.load()));
        }
        for(int s=0;s<numSamples;++s) {
            const float ae=ampAdsr.getNextSample(), fe=filtAdsr.getNextSample();
            if(!ampAdsr.isActive()){active=false;break;}
            float L=0,R=0;
            auto renderOsc=[&](float* phases, int voices, float freq2, float det, float spr, float wt, float lvl, float pan) {
                float oL=0,oR=0;
                for(int u=0;u<voices;++u) {
                    float d=(voices==1)?0.0f:det*((float)u/(voices-1)*2.0f-1.0f);
                    float smp=kGlobalWT.getMorphSample(wt,phases[u]);
                    phases[u]=std::fmod(phases[u]+(freq2+d)/sr,1.0f);
                    float pu=(voices==1)?pan:juce::jlimit(0.0f,1.0f,pan+spr*((float)u/(voices-1)-0.5f));
                    oL+=smp*std::cos(pu*juce::MathConstants<float>::halfPi);
                    oR+=smp*std::sin(pu*juce::MathConstants<float>::halfPi);
                }
                L+=oL*lvl/voices; R+=oR*lvl/voices;
            };
            if(aEn) renderOsc(phaseA,uA,fA,dA,sA,wtA,aLvl,aPan);
            if(bEn) renderOsc(phaseB,uB,fB,dB,sB,wtB,bLvl,bPan);
            if(p.subEnabled.load()){
                float sub=std::sin(juce::MathConstants<float>::twoPi*subPhase)*p.subLevel.load();
                subPhase=std::fmod(subPhase+(bf*0.5f)/sr,1.0f);
                L+=sub; R+=sub;
            }
            if(p.noiseEnabled.load()){float n=(rng.nextFloat()*2-1)*p.noiseLevel.load();L+=n;R+=n;}
            if(fOn){
                float kt=(currentNote-60)*fKt;
                float ev=fEnv*4.0f*fe*12.0f;
                float co=p.filterCutoff.load()*std::pow(2.0f,(kt+ev)/12.0f);
                filter.setCutoffFrequency(juce::jlimit(20.0f,20000.0f,co));
                L=filter.processSample(0,L); R=filter.processSample(1,R);
            }
            out.addSample(0,startSample+s,L*ae*currentVelocity*ml);
            out.addSample(1,startSample+s,R*ae*currentVelocity*ml);
        }
    }
private:
    double sampleRate{44100.0}; bool active{false};
    int currentNote{0}; float currentVelocity{0.0f};
    float phaseA[8]{}, phaseB[8]{}, subPhase{0.0f};
    juce::ADSR ampAdsr, filtAdsr;
    juce::dsp::StateVariableTPTFilter<float> filter;
    juce::Random rng;
};

// ============================================================
// WavetableSynthProcessor
// ============================================================
class WavetableSynthProcessor : public InstrumentProcessor {
public:
    WTParams params;
    WavetableSynthProcessor() = default;

    void prepareToPlay(double sr) override { for(auto& v:voices) v.prepare(sr); }

    void processBlock(juce::AudioBuffer<float>& out, const juce::MidiBuffer& midi) override {
        int n=out.getNumSamples(), pos=0;
        for(const auto m:midi){
            auto msg=m.getMessage(); int sp=m.samplePosition;
            if(sp>pos){renderVoices(out,pos,sp-pos);pos=sp;}
            if(msg.isNoteOn())       handleNoteOn(msg.getNoteNumber(),msg.getFloatVelocity());
            else if(msg.isNoteOff()) handleNoteOff(msg.getNoteNumber());
            else if(msg.isAllNotesOff()){for(auto& v:voices)v.noteOff();}
        }
        if(pos<n) renderVoices(out,pos,n-pos);
    }

    void clear() override { for(auto& v:voices)v.noteOff(); }

    juce::ValueTree saveState() const override {
        juce::ValueTree t("WavetableState");
        auto saveOsc=[&](const juce::String& id,const WTParams::Osc& o){
            juce::ValueTree n(id);
            n.setProperty("en",o.enabled.load(),nullptr); n.setProperty("wt",o.wtPos.load(),nullptr);
            n.setProperty("oct",o.octave.load(),nullptr); n.setProperty("crs",o.coarse.load(),nullptr);
            n.setProperty("fin",o.fine.load(),nullptr);   n.setProperty("lvl",o.level.load(),nullptr);
            n.setProperty("pan",o.pan.load(),nullptr);    n.setProperty("uni",o.unisonVoices.load(),nullptr);
            n.setProperty("det",o.unisonDetune.load(),nullptr); n.setProperty("spr",o.unisonSpread.load(),nullptr);
            t.addChild(n,-1,nullptr);
        };
        saveOsc("A",params.oscA); saveOsc("B",params.oscB);
        t.setProperty("ml",params.masterLevel.load(),nullptr);
        t.setProperty("subE",params.subEnabled.load(),nullptr); t.setProperty("subL",params.subLevel.load(),nullptr);
        t.setProperty("noiE",params.noiseEnabled.load(),nullptr); t.setProperty("noil",params.noiseLevel.load(),nullptr);
        t.setProperty("fE",params.filterEnabled.load(),nullptr); t.setProperty("fT",params.filterType.load(),nullptr);
        t.setProperty("fC",params.filterCutoff.load(),nullptr);  t.setProperty("fR",params.filterRes.load(),nullptr);
        t.setProperty("fEA",params.filterEnvAmt.load(),nullptr); t.setProperty("fKt",params.filterKeytrack.load(),nullptr);
        t.setProperty("aA",params.ampA.load(),nullptr);  t.setProperty("aD",params.ampD.load(),nullptr);
        t.setProperty("aS",params.ampS.load(),nullptr);  t.setProperty("aR",params.ampR.load(),nullptr);
        t.setProperty("fA",params.filtA.load(),nullptr); t.setProperty("fD",params.filtD.load(),nullptr);
        t.setProperty("fS",params.filtS.load(),nullptr); t.setProperty("fRR",params.filtR.load(),nullptr);
        return t;
    }

    void loadState(const juce::ValueTree& t) override {
        auto loadOsc=[&](const juce::String& id,WTParams::Osc& o){
            auto n=t.getChildWithName(id); if(!n.isValid())return;
            o.enabled.store((bool)n.getProperty("en",true));  o.wtPos.store((float)n.getProperty("wt",0.0f));
            o.octave.store((float)n.getProperty("oct",0.0f)); o.coarse.store((float)n.getProperty("crs",0.0f));
            o.fine.store((float)n.getProperty("fin",0.0f));   o.level.store((float)n.getProperty("lvl",0.8f));
            o.pan.store((float)n.getProperty("pan",0.5f));    o.unisonVoices.store((int)n.getProperty("uni",1));
            o.unisonDetune.store((float)n.getProperty("det",0.1f)); o.unisonSpread.store((float)n.getProperty("spr",0.5f));
        };
        loadOsc("A",params.oscA); loadOsc("B",params.oscB);
        params.masterLevel.store((float)t.getProperty("ml",0.8f));
        params.subEnabled.store((bool)t.getProperty("subE",false));  params.subLevel.store((float)t.getProperty("subL",0.4f));
        params.noiseEnabled.store((bool)t.getProperty("noiE",false)); params.noiseLevel.store((float)t.getProperty("noil",0.0f));
        params.filterEnabled.store((bool)t.getProperty("fE",true));  params.filterType.store((int)t.getProperty("fT",0));
        params.filterCutoff.store((float)t.getProperty("fC",8000.0f)); params.filterRes.store((float)t.getProperty("fR",0.707f));
        params.filterEnvAmt.store((float)t.getProperty("fEA",0.0f)); params.filterKeytrack.store((float)t.getProperty("fKt",0.0f));
        params.ampA.store((float)t.getProperty("aA",0.001f)); params.ampD.store((float)t.getProperty("aD",0.1f));
        params.ampS.store((float)t.getProperty("aS",1.0f));   params.ampR.store((float)t.getProperty("aR",0.2f));
        params.filtA.store((float)t.getProperty("fA",0.01f)); params.filtD.store((float)t.getProperty("fD",0.2f));
        params.filtS.store((float)t.getProperty("fS",0.5f));  params.filtR.store((float)t.getProperty("fRR",0.3f));
    }

    std::unique_ptr<juce::Component> createEditor() override;
    juce::String getName() const override { return "WavetableSynth"; }

private:
    void handleNoteOn(int n,float v){
        for(auto& vo:voices){if(!vo.isActive()){vo.noteOn(n,v,params);return;}}
        for(auto& vo:voices){if(vo.getNote()==n){vo.noteOn(n,v,params);return;}}
        voices[0].noteOn(n,v,params);
    }
    void handleNoteOff(int n){for(auto& v:voices)if(v.isActive()&&v.getNote()==n)v.noteOff();}
    void renderVoices(juce::AudioBuffer<float>& b,int s,int n){for(auto& v:voices)if(v.isActive())v.renderNextBlock(b,s,n,params);}
    std::array<WTVoice,8> voices;
};
