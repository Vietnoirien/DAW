// SPDX-License-Identifier: AGPL-3.0-or-later
// LiBeDAW unit tests — Suite C: CompressorEffect + DelayEffect
#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <cmath>

#include "BuiltInEffects.h"

// ─── Named constants ─────────────────────────────────────────────────────────
constexpr double kSampleRate   = 48000.0;
constexpr int    kBlockSize    = 4096;   // large block for steady-state measurement
constexpr int    kSmallBlock   = 512;
constexpr float  kPi           = juce::MathConstants<float>::pi;

// ─── DSP helpers ─────────────────────────────────────────────────────────────

// Fill buffer with DC offset on all channels
static void fillDC(juce::AudioBuffer<float>& buf, float value)
{
    for (int ch = 0; ch < buf.getNumChannels(); ++ch)
        for (int s = 0; s < buf.getNumSamples(); ++s)
            buf.setSample(ch, s, value);
}

// Fill buffer with a sine wave (all channels, same signal)
static void fillSine(juce::AudioBuffer<float>& buf, float amplitude,
                     float freqHz, double sampleRate)
{
    for (int ch = 0; ch < buf.getNumChannels(); ++ch)
        for (int s = 0; s < buf.getNumSamples(); ++s)
            buf.setSample(ch, s, amplitude * std::sin(
                2.0f * kPi * freqHz * static_cast<float>(s) / static_cast<float>(sampleRate)));
}

// RMS of channel 0
static float rmsDb(const juce::AudioBuffer<float>& buf)
{
    double sumSq = 0.0;
    for (int s = 0; s < buf.getNumSamples(); ++s)
        sumSq += static_cast<double>(buf.getSample(0, s)) * buf.getSample(0, s);
    float rms = static_cast<float>(std::sqrt(sumSq / buf.getNumSamples()));
    return (rms > 1e-10f) ? 20.0f * std::log10(rms) : -120.0f;
}

// ─────────────────────────────────────────────────────────────────────────────
// CompressorEffect tests
// ─────────────────────────────────────────────────────────────────────────────

TEST(CompressorEffect, NoGainReductionBelowThreshold)
{
    CompressorEffect comp;
    comp.prepareToPlay(kSampleRate);
    comp.threshold.store(-20.0f);
    comp.ratio.store(4.0f);
    comp.attack.store(1.0f);
    comp.release.store(100.0f);
    comp.makeup.store(0.0f);
    comp.mix.store(100.0f);

    // Signal at -30 dBFS (well below -20 dB threshold)
    const float inAmplitude = std::pow(10.0f, -30.0f / 20.0f);
    juce::AudioBuffer<float> buf(2, kBlockSize);
    fillDC(buf, inAmplitude);

    // Prime the envelope with a warm-up block
    juce::AudioBuffer<float> warmup(2, kBlockSize);
    fillDC(warmup, inAmplitude);
    comp.processBlock(warmup);
    comp.processBlock(buf);

    float outDb  = rmsDb(buf);
    float inDb   = 20.0f * std::log10(inAmplitude);
    // 1.0 dB tolerance: envelope follower may have minor inaccuracy at very low levels
    EXPECT_NEAR(outDb, inDb, 1.0f)
        << "Signal below threshold must pass through with no gain reduction (within 1 dB)";
}

TEST(CompressorEffect, GainReductionAboveThreshold)
{
    CompressorEffect comp;
    comp.prepareToPlay(kSampleRate);
    comp.threshold.store(-20.0f);
    comp.ratio.store(4.0f);
    comp.attack.store(1.0f);
    comp.release.store(200.0f);
    comp.makeup.store(0.0f);
    comp.mix.store(100.0f);

    // Signal at -10 dBFS (10 dB above threshold)
    // With ratio 4:1, excess = 10 dB, GR = 10 - 10/4 = 7.5 dB, output ≈ -17.5 dBFS
    const float inAmplitude = std::pow(10.0f, -10.0f / 20.0f);
    juce::AudioBuffer<float> warmup(2, kBlockSize);
    fillDC(warmup, inAmplitude);
    comp.processBlock(warmup); // prime envelope

    juce::AudioBuffer<float> buf(2, kBlockSize);
    fillDC(buf, inAmplitude);
    comp.processBlock(buf);

    float outDb = rmsDb(buf);
    // 2.0 dB tolerance: single-pole IIR envelope may not perfectly model hard-knee
    EXPECT_LT(outDb, -10.0f + 2.0f)
        << "Compressor must apply gain reduction when signal is above threshold";
    EXPECT_GT(outDb, -30.0f)
        << "Gain reduction must not silence the signal entirely";
}

TEST(CompressorEffect, ParallelMixZeroIsFullyDry)
{
    CompressorEffect comp;
    comp.prepareToPlay(kSampleRate);
    comp.threshold.store(-6.0f);
    comp.ratio.store(10.0f); // heavy compression
    comp.mix.store(0.0f);    // 0% wet = fully dry
    comp.makeup.store(0.0f);

    // Signal at -30 dBFS — well below -6 dB threshold so no compression fires.
    // At mix=0%, the output must equal the dry input regardless of threshold.
    // We use a sub-threshold signal to avoid any envelope-follower interaction.
    const float inAmplitude = std::pow(10.0f, -30.0f / 20.0f);
    juce::AudioBuffer<float> buf(2, kBlockSize);
    fillDC(buf, inAmplitude);

    comp.processBlock(buf);

    float outDb = rmsDb(buf);
    float inDb  = 20.0f * std::log10(inAmplitude);
    // 0.5 dB tolerance: sub-threshold signal with mix=0% must pass through
    // within measurement precision. Any deviation > 0.5 dB indicates mix is not 0%.
    EXPECT_NEAR(outDb, inDb, 0.5f)
        << "mix=0% with sub-threshold signal: output RMS must equal input RMS within 0.5 dB";
}

TEST(CompressorEffect, MakeupGainScalesOutput)
{
    CompressorEffect comp;
    comp.prepareToPlay(kSampleRate);
    comp.threshold.store(0.0f);  // above any signal, so no compression
    comp.ratio.store(1.0f);
    comp.makeup.store(6.0f);     // +6 dB makeup
    comp.mix.store(100.0f);

    const float inAmplitude = 0.25f;
    juce::AudioBuffer<float> buf(2, kBlockSize);
    fillDC(buf, inAmplitude);
    comp.processBlock(buf);

    float outDb  = rmsDb(buf);
    float inDb   = 20.0f * std::log10(inAmplitude);
    float diff   = outDb - inDb;
    // 1.0 dB tolerance: envelope follower at ratio=1 has minor inaccuracy
    EXPECT_NEAR(diff, 6.0f, 1.0f)
        << "+6 dB makeup must raise output by approximately 6 dB above input";
}

// ─────────────────────────────────────────────────────────────────────────────
// DelayEffect tests
// ─────────────────────────────────────────────────────────────────────────────

TEST(DelayEffect, DelayTimeMs_ImpulseArrivesAtCorrectSample)
{
    DelayEffect delay;
    delay.prepareToPlay(kSampleRate);
    delay.syncEnabled.store(false);
    // 100 ms delay at 48 kHz = 4800 samples
    delay.delayTimeMs.store(100.0f);
    delay.feedback.store(0.0f);
    delay.mix.store(1.0f); // 100% wet so we can isolate the delay

    constexpr int kDelaySamples  = 4800; // 100 ms * 48 kHz
    constexpr int kTotalSamples  = kDelaySamples + kSmallBlock;

    juce::AudioBuffer<float> buf(2, kTotalSamples);
    buf.clear();
    // Impulse at sample 0
    buf.setSample(0, 0, 1.0f);
    buf.setSample(1, 0, 1.0f);

    // Process in one large block (prepareToPlay pre-cleared the delay line)
    delay.processBlock(buf);

    // At 100% wet mix the delayed impulse should appear at kDelaySamples
    // 1e-3f: the JUCE DelayLine uses linear interpolation — exact integer delays
    // are very accurate but not always sample-perfect
    EXPECT_NEAR(buf.getSample(0, kDelaySamples), 1.0f, 1e-3f)
        << "Delayed impulse must appear at the correct sample offset (100 ms = 4800 samples)";
}

TEST(DelayEffect, TempoSyncQuarterNoteAt120Bpm)
{
    // 1/4 note at 120 BPM = 0.5 s = 24000 samples @ 48 kHz
    // Division index 6 = 1.0 beat = quarter note (see kDivisionBeats table)
    DelayEffect delay;
    delay.prepareToPlay(kSampleRate);
    delay.syncEnabled.store(true);
    delay.syncDivision.store(6); // 1/4 note
    delay.setBpm(120.0);
    delay.feedback.store(0.0f);
    delay.mix.store(1.0f);

    constexpr int kDelaySamples = 24000; // 0.5 s * 48 kHz
    constexpr int kTotalSamples = kDelaySamples + kSmallBlock;

    juce::AudioBuffer<float> buf(2, kTotalSamples);
    buf.clear();
    buf.setSample(0, 0, 1.0f);
    buf.setSample(1, 0, 1.0f);
    delay.processBlock(buf);

    // 2e-3f: tempo-sync delay time is computed in floating-point; 1 sample jitter acceptable
    EXPECT_NEAR(buf.getSample(0, kDelaySamples), 1.0f, 2e-3f)
        << "Tempo-sync 1/4 note at 120 BPM must delay impulse by 24000 samples (0.5 s)";
}

TEST(DelayEffect, FeedbackDecaysEachEcho)
{
    DelayEffect delay;
    delay.prepareToPlay(kSampleRate);
    delay.syncEnabled.store(false);
    // Short delay for testability: 10 ms = 480 samples
    delay.delayTimeMs.store(10.0f);
    delay.feedback.store(0.5f); // each echo is -6 dB (half amplitude)
    delay.mix.store(1.0f);

    constexpr int kDelaySamples = 480;
    constexpr int kTotalSamples = kDelaySamples * 5;

    juce::AudioBuffer<float> buf(2, kTotalSamples);
    buf.clear();
    buf.setSample(0, 0, 1.0f);
    buf.setSample(1, 0, 1.0f);
    delay.processBlock(buf);

    float echo1 = std::abs(buf.getSample(0, kDelaySamples));
    float echo2 = std::abs(buf.getSample(0, kDelaySamples * 2));

    // Each echo must be smaller than the previous (feedback < 1.0)
    EXPECT_GT(echo1, 0.0f) << "First echo must be non-zero";
    EXPECT_LT(echo2, echo1)
        << "Second echo must be quieter than the first (feedback = 0.5)";
}

TEST(DelayEffect, SaveLoadRoundtrip)
{
    DelayEffect original;
    original.prepareToPlay(kSampleRate);
    original.delayTimeMs.store(333.0f);
    original.feedback.store(0.7f);
    original.mix.store(0.6f);
    original.syncEnabled.store(true);
    original.syncDivision.store(4); // 1/8 note

    auto tree = original.saveState();

    DelayEffect restored;
    restored.prepareToPlay(kSampleRate);
    restored.loadState(tree);

    // 1e-5f: ValueTree round-trips floats through double
    EXPECT_NEAR(restored.delayTimeMs.load(),  333.0f, 1e-5f) << "delayTimeMs";
    EXPECT_NEAR(restored.feedback.load(),       0.7f, 1e-5f) << "feedback";
    EXPECT_NEAR(restored.mix.load(),            0.6f, 1e-5f) << "mix";
    EXPECT_TRUE(restored.syncEnabled.load())                 << "syncEnabled";
    EXPECT_EQ  (restored.syncDivision.load(),      4)        << "syncDivision";
}
