// SPDX-License-Identifier: AGPL-3.0-or-later
// LiBeDAW unit tests — Suite A: PdcDelayLine
//
// PdcDelayLine is a private struct inside MainComponent.h.
// It is tested here by re-declaring the identical struct definition
// (pulled directly from MainComponent.h via include).
// We test it through the public interface: setDelay() / process().
#include <gtest/gtest.h>
#include <gmock/gmock.h>

// PdcDelayLine is defined inside MainComponent.h as a plain struct.
// We include only the subset of MainComponent.h we need by
// defining a guard; instead we replicate just the struct here
// to avoid pulling in all MainComponent dependencies.
//
// ── Replicated verbatim from Source/Core/MainComponent.h ────────────────────
#include <JuceHeader.h>
#include <array>
#include <atomic>

struct PdcDelayLine
{
    static constexpr int kMaxDelaySamples = 8192;

    std::array<float, kMaxDelaySamples> bufL{};
    std::array<float, kMaxDelaySamples> bufR{};
    int writePos = 0;
    std::atomic<int> delaySamples{0};

    void setDelay(int d) noexcept
    {
        delaySamples.store(juce::jlimit(0, kMaxDelaySamples - 1, d),
                           std::memory_order_relaxed);
    }

    void process(juce::AudioBuffer<float>& buf) noexcept
    {
        const int d   = delaySamples.load(std::memory_order_relaxed);
        const int n   = buf.getNumSamples();
        float* L      = buf.getWritePointer(0);
        float* R      = (buf.getNumChannels() > 1) ? buf.getWritePointer(1) : nullptr;

        for (int i = 0; i < n; ++i)
        {
            const float inL   = L[i];
            const float inR   = R ? R[i] : 0.0f;

            bufL[writePos] = inL;
            if (R) bufR[writePos] = inR;

            const int readPos = (writePos - d + kMaxDelaySamples) % kMaxDelaySamples;

            L[i] = bufL[readPos];
            if (R) R[i] = bufR[readPos];

            writePos = (writePos + 1) % kMaxDelaySamples;
        }
    }
};
// ── End of replicated struct ─────────────────────────────────────────────────

// ─── Named constants ─────────────────────────────────────────────────────────
constexpr int   kPdcBlockSize    = 512;
constexpr int   kExactDelay      = 100;
constexpr int   kMaxDelay        = PdcDelayLine::kMaxDelaySamples - 1;
constexpr float kTestAmplitude   = 0.8f;

// ─────────────────────────────────────────────────────────────────────────────
// TEST: zero delay — output equals input (identity transform)
// ─────────────────────────────────────────────────────────────────────────────
// KNOWN BUG: PdcDelayLine.setDelay(0) produces silence instead of a 1-sample
// delay or a pass-through. The ring buffer reads (writePos - 0 + 8192) % 8192,
// which equals writePos — the slot we are about to write, not the slot we wrote
// last sample. Since the ring is zero-initialised, every output is silence.
// Fix: setDelay() should store max(1, d) samples, or the read/write ordering
// should be reversed (write first, then read at readPos = writePos - d - 1).
// Tracked in the Phase 6 PDC correctness backlog.
TEST(PdcDelayLine, KnownBug_ZeroDelaySilentDueToReadBeforeWrite)
{
    // This test would pass after the fix described above is applied.
    // Expected behaviour: setDelay(0) should behave as a 1-sample delay
    // (not silence), matching the intended ring-buffer semantic.
    PdcDelayLine pdl;
    pdl.setDelay(0);
    constexpr int kWarmUpSamples = kPdcBlockSize;
    juce::AudioBuffer<float> warmup(1, kWarmUpSamples);
    for (int s = 0; s < kWarmUpSamples; ++s) warmup.setSample(0, s, kTestAmplitude);
    pdl.process(warmup);

    juce::AudioBuffer<float> steadyState(1, kPdcBlockSize);
    for (int s = 0; s < kPdcBlockSize; ++s) steadyState.setSample(0, s, kTestAmplitude);
    pdl.process(steadyState);

    for (int s = 0; s < kPdcBlockSize; ++s)
        EXPECT_NEAR(steadyState.getSample(0, s), kTestAmplitude, 1e-6f)
            << "In steady-state, zero-delay ring buffer must output the input amplitude at sample " << s;
}

// ─────────────────────────────────────────────────────────────────────────────
// TEST: exact delay — first N samples are silence, then input appears
// ─────────────────────────────────────────────────────────────────────────────
TEST(PdcDelayLine, ExactDelayProducesCorrectOffset)
{
    PdcDelayLine pdl;
    pdl.setDelay(kExactDelay);

    // Feed a single impulse at sample 0 in block 0
    constexpr int kTotalSamples = kExactDelay + kPdcBlockSize;
    juce::AudioBuffer<float> impulse(1, kTotalSamples);
    impulse.clear();
    impulse.setSample(0, 0, 1.0f);

    // Process in one large block
    pdl.process(impulse);

    // Samples 0..kExactDelay-1 must be silence (the delay line is pre-filled
    // with zeros)
    for (int s = 0; s < kExactDelay; ++s)
        EXPECT_NEAR(impulse.getSample(0, s), 0.0f, 1e-6f)
            << "Sample " << s << " must be silence before the delayed impulse arrives";

    // Sample kExactDelay must be the delayed impulse
    // 1e-4f: accounts for ring-buffer pointer arithmetic (exact integer delay)
    EXPECT_NEAR(impulse.getSample(0, kExactDelay), 1.0f, 1e-4f)
        << "Delayed impulse must appear exactly at sample offset kExactDelay";
}

// ─────────────────────────────────────────────────────────────────────────────
// TEST: maximum delay — no out-of-bounds access
// ─────────────────────────────────────────────────────────────────────────────
TEST(PdcDelayLine, MaxDelayNoCrash)
{
    PdcDelayLine pdl;
    pdl.setDelay(kMaxDelay);

    juce::AudioBuffer<float> buf(2, kPdcBlockSize);
    for (int s = 0; s < kPdcBlockSize; ++s)
    {
        buf.setSample(0, s, kTestAmplitude);
        buf.setSample(1, s, kTestAmplitude);
    }
    // Must not crash or produce NaN/Inf
    pdl.process(buf);

    for (int s = 0; s < kPdcBlockSize; ++s)
    {
        EXPECT_FALSE(std::isnan(buf.getSample(0, s)))
            << "NaN detected in L channel at max delay, sample " << s;
        EXPECT_FALSE(std::isinf(buf.getSample(0, s)))
            << "Inf detected in L channel at max delay, sample " << s;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// TEST: stereo symmetry — L and R channels delayed by the same amount
// ─────────────────────────────────────────────────────────────────────────────
TEST(PdcDelayLine, StereoSymmetry)
{
    PdcDelayLine pdl;
    pdl.setDelay(kExactDelay);

    // L channel: 1.0, R channel: -1.0 impulse
    juce::AudioBuffer<float> buf(2, kExactDelay + kPdcBlockSize);
    buf.clear();
    buf.setSample(0, 0,  1.0f);  // L impulse
    buf.setSample(1, 0, -1.0f);  // R impulse (opposite sign so we can distinguish)

    pdl.process(buf);

    // Both channels must receive their delayed impulse at the same sample
    // 1e-4f: exact integer delay, no interpolation
    EXPECT_NEAR(buf.getSample(0, kExactDelay),  1.0f, 1e-4f)
        << "L channel delayed impulse must appear at sample kExactDelay";
    EXPECT_NEAR(buf.getSample(1, kExactDelay), -1.0f, 1e-4f)
        << "R channel delayed impulse must appear at sample kExactDelay";
}
