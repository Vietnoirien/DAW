// SPDX-License-Identifier: AGPL-3.0-or-later
// LiBeDAW unit tests — Suite A: AppAudioBuffer
#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include "AppAudioBuffer.h"

// ─── Named constants ─────────────────────────────────────────────────────────
constexpr int    kNumChannels  = 2;
constexpr int    kBlockSize    = 512;
constexpr double kSampleRate   = 48000.0;
// AppAudioBuffer ring holds RING_CAPACITY_BLOCKS hardware blocks
constexpr int    kRingCapacity = AppAudioBuffer::RING_CAPACITY_BLOCKS;

// ─── Helpers ──────────────────────────────────────────────────────────────────
static juce::AudioBuffer<float> makeSineBlock(int numChannels, int numSamples,
                                               float freq, double sampleRate)
{
    juce::AudioBuffer<float> buf(numChannels, numSamples);
    for (int ch = 0; ch < numChannels; ++ch)
        for (int s = 0; s < numSamples; ++s)
            buf.setSample(ch, s,
                std::sin(2.0f * juce::MathConstants<float>::pi
                         * freq * static_cast<float>(s) / static_cast<float>(sampleRate)));
    return buf;
}

static float peakAmplitude(const juce::AudioBuffer<float>& buf, int ch = 0)
{
    float peak = 0.0f;
    for (int s = 0; s < buf.getNumSamples(); ++s)
        peak = std::max(peak, std::abs(buf.getSample(ch, s)));
    return peak;
}

// ─────────────────────────────────────────────────────────────────────────────
// TEST: prepare() configures basic properties correctly
// ─────────────────────────────────────────────────────────────────────────────
TEST(AppAudioBuffer, PrepareConfiguresProperties)
{
    AppAudioBuffer buf;
    buf.prepare(kNumChannels, kBlockSize, kSampleRate);

    EXPECT_EQ(buf.getNumChannels(), kNumChannels)
        << "getNumChannels() must return the value passed to prepare()";
    EXPECT_EQ(buf.getBlockSize(), kBlockSize)
        << "getBlockSize() must return the value passed to prepare()";
    EXPECT_NEAR(buf.getSampleRate(), kSampleRate, 1e-3)
        << "getSampleRate() must return the value passed to prepare()";
}

// ─────────────────────────────────────────────────────────────────────────────
// TEST: write one sine block, read it back sample-accurately
// ─────────────────────────────────────────────────────────────────────────────
TEST(AppAudioBuffer, WriteThenReadPreservesSamples)
{
    AppAudioBuffer ringBuf;
    ringBuf.prepare(kNumChannels, kBlockSize, kSampleRate);

    constexpr float kFreq = 440.0f;
    auto src = makeSineBlock(kNumChannels, kBlockSize, kFreq, kSampleRate);
    ringBuf.writeBlock(src);

    juce::AudioBuffer<float> dest(kNumChannels, kBlockSize);
    ringBuf.readBlock(dest, kBlockSize);

    // Tolerance: floating-point copy through ring buffer must be exact (no DSP)
    for (int ch = 0; ch < kNumChannels; ++ch)
        for (int s = 0; s < kBlockSize; ++s)
            EXPECT_NEAR(dest.getSample(ch, s), src.getSample(ch, s), 1e-6f)
                << "Sample mismatch at ch=" << ch << " s=" << s;
}

// ─────────────────────────────────────────────────────────────────────────────
// TEST: reading without prior write gives silence and increments underrun counter
// ─────────────────────────────────────────────────────────────────────────────
TEST(AppAudioBuffer, UnderrunProducesSilenceAndIncrementsCounter)
{
    AppAudioBuffer ringBuf;
    ringBuf.prepare(kNumChannels, kBlockSize, kSampleRate);

    // Pre-read the underrun counter to clear any residual
    (void)ringBuf.getAndResetUnderrunCount();

    juce::AudioBuffer<float> dest(kNumChannels, kBlockSize);
    dest.clear();
    // Fill dest with non-zero data first so we can verify it was overwritten
    for (int ch = 0; ch < kNumChannels; ++ch)
        for (int s = 0; s < kBlockSize; ++s)
            dest.setSample(ch, s, 1.0f);

    ringBuf.readBlock(dest, kBlockSize); // No prior write → underrun

    EXPECT_NEAR(peakAmplitude(dest, 0), 0.0f, 1e-6f)
        << "Underrun must produce silence (channel 0)";
    EXPECT_NEAR(peakAmplitude(dest, 1), 0.0f, 1e-6f)
        << "Underrun must produce silence (channel 1)";

    int underruns = ringBuf.getAndResetUnderrunCount();
    EXPECT_EQ(underruns, 1)
        << "Underrun counter must be incremented exactly once after one underrun read";
}

// ─────────────────────────────────────────────────────────────────────────────
// TEST: multi-block round-trip — write RING_CAPACITY_BLOCKS, read all back in order
// ─────────────────────────────────────────────────────────────────────────────
TEST(AppAudioBuffer, MultiBlockRoundtripPreservesOrder)
{
    AppAudioBuffer ringBuf;
    ringBuf.prepare(kNumChannels, kBlockSize, kSampleRate);

    // Write kRingCapacity - 1 distinct blocks (one slot must remain empty in
    // a ring buffer to distinguish full from empty)
    constexpr int kWriteBlocks = kRingCapacity - 1;
    std::vector<juce::AudioBuffer<float>> written;

    for (int b = 0; b < kWriteBlocks; ++b)
    {
        // Each block has a unique DC offset (b+1)/kWriteBlocks so we can identify it
        juce::AudioBuffer<float> src(kNumChannels, kBlockSize);
        const float dc = static_cast<float>(b + 1) / static_cast<float>(kWriteBlocks);
        src.clear();
        for (int ch = 0; ch < kNumChannels; ++ch)
            for (int s = 0; s < kBlockSize; ++s)
                src.setSample(ch, s, dc);

        ASSERT_TRUE(ringBuf.hasRoomToWrite())
            << "Ring buffer must have room before filling to kRingCapacity-1";
        ringBuf.writeBlock(src);
        written.push_back(src);
    }

    // Read and verify order and content
    for (int b = 0; b < kWriteBlocks; ++b)
    {
        juce::AudioBuffer<float> dest(kNumChannels, kBlockSize);
        ringBuf.readBlock(dest, kBlockSize);

        const float expectedDc = static_cast<float>(b + 1)
                                / static_cast<float>(kWriteBlocks);
        // 1e-6f: tolerance for exact float copy through the ring
        EXPECT_NEAR(dest.getSample(0, 0), expectedDc, 1e-6f)
            << "Block " << b << " DC offset mismatch — FIFO order violated";
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// TEST: getAndResetUnderrunCount returns 0 when there were no underruns
// ─────────────────────────────────────────────────────────────────────────────
TEST(AppAudioBuffer, UnderrunCountIsZeroWithoutUnderruns)
{
    AppAudioBuffer ringBuf;
    ringBuf.prepare(kNumChannels, kBlockSize, kSampleRate);

    auto src = makeSineBlock(kNumChannels, kBlockSize, 440.0f, kSampleRate);
    ringBuf.writeBlock(src);

    juce::AudioBuffer<float> dest(kNumChannels, kBlockSize);
    ringBuf.readBlock(dest, kBlockSize);

    EXPECT_EQ(ringBuf.getAndResetUnderrunCount(), 0)
        << "No underruns should be counted when write precedes read";
}

// ─────────────────────────────────────────────────────────────────────────────
// TEST: getAndResetUnderrunCount clears the counter after reading
// ─────────────────────────────────────────────────────────────────────────────
TEST(AppAudioBuffer, UnderrunCountResetAfterRead)
{
    AppAudioBuffer ringBuf;
    ringBuf.prepare(kNumChannels, kBlockSize, kSampleRate);

    juce::AudioBuffer<float> dest(kNumChannels, kBlockSize);
    ringBuf.readBlock(dest, kBlockSize); // underrun #1
    ringBuf.readBlock(dest, kBlockSize); // underrun #2

    int first = ringBuf.getAndResetUnderrunCount();
    EXPECT_EQ(first, 2) << "Two underruns must be counted";

    int second = ringBuf.getAndResetUnderrunCount();
    EXPECT_EQ(second, 0) << "Counter must be 0 after being reset";
}
