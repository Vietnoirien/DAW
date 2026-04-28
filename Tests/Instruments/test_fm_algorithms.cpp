// SPDX-License-Identifier: AGPL-3.0-or-later
// LiBeDAW unit tests — Suite C: FM Algorithm Table + FMSynthProcessor
#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "FMSynthProcessor.h"

constexpr double kSampleRate     = 48000.0;
constexpr int    kBlockSize      = 512;
constexpr int    kNumAlgorithms  = 32;
constexpr int    kNumOperators   = 4;
constexpr float  kNoteOnVelocity = 0.8f;
constexpr int    kMidiMiddleC    = 60;
constexpr float  kAmpTolerance   = 1e-3f;

static juce::AudioBuffer<float> makeEmptyOutputBuffer()
{
    juce::AudioBuffer<float> buf(2, kBlockSize);
    buf.clear();
    return buf;
}

static float peakAmplitude(const juce::AudioBuffer<float>& buf)
{
    float peak = 0.0f;
    for (int ch = 0; ch < buf.getNumChannels(); ++ch)
        for (int s = 0; s < buf.getNumSamples(); ++s)
            peak = std::max(peak, std::abs(buf.getSample(ch, s)));
    return peak;
}

// ── Algorithm table ───────────────────────────────────────────────────────────

TEST(FMAlgorithmTable, AllUnique)
{
    for (int i = 0; i < kNumAlgorithms; ++i)
        for (int j = i + 1; j < kNumAlgorithms; ++j)
        {
            bool modulatedByMatch = true, isCarrierMatch = true;
            for (int op = 0; op < kNumOperators; ++op)
            {
                if (kDX7Algorithms[i].modulatedBy[op] != kDX7Algorithms[j].modulatedBy[op])
                    modulatedByMatch = false;
                if (kDX7Algorithms[i].isCarrier[op] != kDX7Algorithms[j].isCarrier[op])
                    isCarrierMatch = false;
            }
            EXPECT_FALSE(modulatedByMatch && isCarrierMatch)
                << "Alg " << i << " and Alg " << j << " share identical topology (Phase 8.3 blocker)";
        }
}

TEST(FMAlgorithmTable, EveryAlgorithmHasAtLeastOneCarrier)
{
    for (int i = 0; i < kNumAlgorithms; ++i)
    {
        bool hasCarrier = false;
        for (int op = 0; op < kNumOperators; ++op)
            if (kDX7Algorithms[i].isCarrier[op]) hasCarrier = true;
        EXPECT_TRUE(hasCarrier) << "Algorithm " << i << " has no carrier — would produce silence";
    }
}

TEST(FMAlgorithmTable, NoSelfFeedbackViaModulatedBy)
{
    for (int i = 0; i < kNumAlgorithms; ++i)
        for (int op = 0; op < kNumOperators; ++op)
            EXPECT_EQ(kDX7Algorithms[i].modulatedBy[op] & (1 << op), 0)
                << "Algorithm " << i << " op " << op << " has self-modulation in modulatedBy bitmask";
}

// ── FMSynthProcessor ─────────────────────────────────────────────────────────

TEST(FMSynth, SilentWithNoNotes)
{
    FMSynthProcessor synth;
    synth.prepareToPlay(kSampleRate);
    auto out = makeEmptyOutputBuffer();
    juce::MidiBuffer empty;
    synth.processBlock(out, empty);
    EXPECT_NEAR(peakAmplitude(out), 0.0f, kAmpTolerance)
        << "FMSynth with no active voices must produce silence";
}

TEST(FMSynth, NoteOnProducesAudio)
{
    FMSynthProcessor synth;
    synth.prepareToPlay(kSampleRate);
    juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(1, kMidiMiddleC, kNoteOnVelocity), 0);
    auto out = makeEmptyOutputBuffer();
    synth.processBlock(out, midi);
    EXPECT_GT(peakAmplitude(out), kAmpTolerance) << "FMSynth must produce audio after NoteOn";
}

TEST(FMSynth, NoteOffEntersReleaseNotImmediatelySilent)
{
    FMSynthProcessor synth;
    synth.prepareToPlay(kSampleRate);
    synth.params.ampRelease.store(0.5f);
    juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn (1, kMidiMiddleC, kNoteOnVelocity), 0);
    midi.addEvent(juce::MidiMessage::noteOff(1, kMidiMiddleC, 0.0f),           256);
    auto out = makeEmptyOutputBuffer();
    synth.processBlock(out, midi);
    EXPECT_GT(peakAmplitude(out), kAmpTolerance)
        << "Voice must still produce audio in the same block as NoteOff (release phase active)";
}

TEST(FMSynth, PolyphonyLimitVoiceSteal)
{
    FMSynthProcessor synth;
    synth.prepareToPlay(kSampleRate);
    juce::MidiBuffer midi;
    for (int i = 0; i < 9; ++i)
        midi.addEvent(juce::MidiMessage::noteOn(1, 48 + i, kNoteOnVelocity), i);
    auto out = makeEmptyOutputBuffer();
    EXPECT_NO_FATAL_FAILURE(synth.processBlock(out, midi))
        << "9 simultaneous NoteOns (voice count = 8) must not crash";
}

TEST(FMSynth, AllNotesOffSilencesVoicesEventually)
{
    FMSynthProcessor synth;
    synth.prepareToPlay(kSampleRate);
    synth.params.ampRelease.store(0.001f); // 1 ms release
    juce::MidiBuffer midi1;
    for (int i = 0; i < 4; ++i)
        midi1.addEvent(juce::MidiMessage::noteOn(1, 60 + i, kNoteOnVelocity), i);
    auto out1 = makeEmptyOutputBuffer();
    synth.processBlock(out1, midi1);

    juce::MidiBuffer midi2;
    midi2.addEvent(juce::MidiMessage::allNotesOff(1), 0);
    auto out2 = makeEmptyOutputBuffer();
    synth.processBlock(out2, midi2);

    // Process 10 silent blocks — 1 ms release should expire
    for (int b = 0; b < 10; ++b) {
        juce::MidiBuffer empty;
        auto o = makeEmptyOutputBuffer();
        synth.processBlock(o, empty);
    }
    auto finalOut = makeEmptyOutputBuffer();
    juce::MidiBuffer empty;
    synth.processBlock(finalOut, empty);
    // 0.01f: ADSR release may leave a tiny tail; strict silence unreasonable here
    EXPECT_LT(peakAmplitude(finalOut), 0.01f)
        << "After AllNotesOff + 10 blocks with 1ms release, output must be near silence";
}

TEST(FMSynth, MpePitchBendProducesAudio)
{
    FMSynthProcessor synth;
    synth.prepareToPlay(kSampleRate);
    synth.params.mpeBendRange.store(12.0f);
    juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(1, kMidiMiddleC, kNoteOnVelocity), 0);
    midi.addEvent(juce::MidiMessage::pitchWheel(1, 16383), 1);
    auto out = makeEmptyOutputBuffer();
    synth.processBlock(out, midi);
    EXPECT_GT(peakAmplitude(out), kAmpTolerance)
        << "FMSynth must produce audio after MPE pitch bend";
}

TEST(FMSynth, SaveLoadRoundtrip)
{
    FMSynthProcessor original;
    original.prepareToPlay(kSampleRate);
    original.params.algorithm.store(7);
    original.params.masterLevel.store(0.6f);
    original.params.ampAttack.store(0.05f);
    original.params.ampRelease.store(1.2f);
    original.params.filterEnabled.store(true);
    original.params.filterCutoff.store(3500.0f);
    original.params.mpeBendRange.store(24.0f);
    original.params.ops[0].ratio.store(2.0f);
    original.params.ops[3].enabled.store(false);

    auto tree = original.saveState();
    FMSynthProcessor restored;
    restored.prepareToPlay(kSampleRate);
    restored.loadState(tree);

    // 1e-5f: ValueTree round-trips floats through double — tiny epsilon expected
    EXPECT_EQ  (restored.params.algorithm.load(),      7)       << "algorithm";
    EXPECT_NEAR(restored.params.masterLevel.load(),    0.6f,    1e-5f) << "masterLevel";
    EXPECT_NEAR(restored.params.ampAttack.load(),      0.05f,   1e-5f) << "ampAttack";
    EXPECT_NEAR(restored.params.ampRelease.load(),     1.2f,    1e-5f) << "ampRelease";
    EXPECT_TRUE(restored.params.filterEnabled.load())                  << "filterEnabled";
    EXPECT_NEAR(restored.params.filterCutoff.load(),   3500.0f, 1e-3f) << "filterCutoff";
    EXPECT_NEAR(restored.params.mpeBendRange.load(),   24.0f,   1e-5f) << "mpeBendRange";
    EXPECT_NEAR(restored.params.ops[0].ratio.load(),   2.0f,    1e-5f) << "ops[0].ratio";
    EXPECT_FALSE(restored.params.ops[3].enabled.load())                << "ops[3].enabled";
}
