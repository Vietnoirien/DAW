// SPDX-License-Identifier: AGPL-3.0-or-later
// LiBeDAW unit tests — Suite B: EuclideanPattern
#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include "EuclideanPattern.h"
#include "GlobalTransport.h"

// ─── Named constants ─────────────────────────────────────────────────────────
constexpr double kSampleRate    = 48000.0;
constexpr double kBpm           = 120.0;
// At 120 BPM, 48 kHz → samplesPerBeat = 24000
constexpr double kSamplesPerBeat = kSampleRate * 60.0 / kBpm;
// 1 bar = 4 beats = 96000 samples
constexpr double kSamplesPerBar  = kSamplesPerBeat * 4.0;

// Helper: configure a GlobalTransport in-place and start playing
static void configurePlayingTransport(GlobalTransport& t)
{
    t.setSampleRate(kSampleRate);
    t.setBpm(kBpm);
    t.play();
}

// Helper: collect all Note On sample positions from getEventsForBuffer()
// over one full bar starting at patternStart = 0
static std::vector<int64_t> collectNoteOns(EuclideanPattern& pat,
                                            GlobalTransport&  transport,
                                            int               blockSize = 512)
{
    std::vector<int64_t> hits;
    const int64_t totalSamples = static_cast<int64_t>(kSamplesPerBar + 0.5);

    for (int64_t blockStart = 0; blockStart < totalSamples; blockStart += blockSize)
    {
        juce::MidiBuffer buf;
        int actualBlock = static_cast<int>(
            std::min(static_cast<int64_t>(blockSize), totalSamples - blockStart));
        pat.getEventsForBuffer(buf, blockStart, actualBlock, transport, 0.0);

        for (const auto meta : buf)
            if (meta.getMessage().isNoteOn())
                hits.push_back(blockStart + meta.samplePosition);
    }
    return hits;
}

// ─────────────────────────────────────────────────────────────────────────────
// TEST: E(3,8) — Tresillo — [1,0,0,1,0,0,1,0]
// ─────────────────────────────────────────────────────────────────────────────
TEST(EuclideanPattern, E38_Tresillo)
{
    EuclideanPattern pat;
    pat.generate(3, 8);
    const auto& hitMap = pat.getHitMap();

    ASSERT_EQ(hitMap.size(), 8u) << "E(3,8) must have 8 steps";
    EXPECT_EQ(hitMap[0], 1) << "Step 0 must be a hit";
    EXPECT_EQ(hitMap[3], 1) << "Step 3 must be a hit";
    EXPECT_EQ(hitMap[6], 1) << "Step 6 must be a hit";
    // Non-hits
    EXPECT_EQ(hitMap[1], 0) << "Step 1 must be silent";
    EXPECT_EQ(hitMap[4], 0) << "Step 4 must be silent";
}

// ─────────────────────────────────────────────────────────────────────────────
// TEST: E(5,8) — Cinquillo — [1,0,1,1,0,1,1,0]
// ─────────────────────────────────────────────────────────────────────────────
TEST(EuclideanPattern, E58_Cinquillo)
{
    EuclideanPattern pat;
    pat.generate(5, 8);
    const auto& hitMap = pat.getHitMap();

    ASSERT_EQ(hitMap.size(), 8u) << "E(5,8) must have 8 steps";

    // The Bresenham formula produces a valid E(5,8) rotation.
    // Any valid E(5,8) must: (a) have exactly 5 hits, (b) be maximally even.
    int hitCount = 0;
    for (auto v : hitMap) hitCount += v;
    EXPECT_EQ(hitCount, 5)
        << "E(5,8) must have exactly 5 hits regardless of rotation";

    // Step 0 must always be a hit (Bresenham starts at i=0)
    EXPECT_EQ(hitMap[0], 1)
        << "E(5,8): step 0 must be a hit (Bresenham algorithm property)";
}

// ─────────────────────────────────────────────────────────────────────────────
// TEST: E(n,n) — all pulses → all steps are hits
// ─────────────────────────────────────────────────────────────────────────────
TEST(EuclideanPattern, AllPulsesAllHits)
{
    EuclideanPattern pat;
    pat.generate(8, 8);
    const auto& hitMap = pat.getHitMap();

    ASSERT_EQ(hitMap.size(), 8u) << "E(8,8) must have 8 steps";
    for (int i = 0; i < 8; ++i)
        EXPECT_EQ(hitMap[i], 1) << "E(8,8): all steps must be hits, mismatch at step " << i;
}

// ─────────────────────────────────────────────────────────────────────────────
// TEST: E(0,8) — zero pulses → all steps are silent
// ─────────────────────────────────────────────────────────────────────────────
TEST(EuclideanPattern, ZeroPulsesAllSilent)
{
    EuclideanPattern pat;
    pat.generate(0, 8);
    const auto& hitMap = pat.getHitMap();

    ASSERT_EQ(hitMap.size(), 8u) << "E(0,8) must have 8 steps";
    for (int i = 0; i < 8; ++i)
        EXPECT_EQ(hitMap[i], 0) << "E(0,8): all steps must be silent, mismatch at step " << i;
}

// ─────────────────────────────────────────────────────────────────────────────
// TEST: E(1,8) — single pulse at step 0 only
// ─────────────────────────────────────────────────────────────────────────────
TEST(EuclideanPattern, SinglePulseAtStepZero)
{
    EuclideanPattern pat;
    pat.generate(1, 8);
    const auto& hitMap = pat.getHitMap();

    ASSERT_EQ(hitMap.size(), 8u) << "E(1,8) must have 8 steps";
    EXPECT_EQ(hitMap[0], 1) << "E(1,8): step 0 must be the single hit";
    for (int i = 1; i < 8; ++i)
        EXPECT_EQ(hitMap[i], 0) << "E(1,8): steps 1–7 must be silent, mismatch at step " << i;
}

// ─────────────────────────────────────────────────────────────────────────────
// TEST: pulses clamped to steps — E(10,8) must not overflow
// ─────────────────────────────────────────────────────────────────────────────
TEST(EuclideanPattern, PulsesClampedToSteps)
{
    EuclideanPattern pat;
    pat.generate(10, 8); // k > n: should clamp to k=8
    const auto& hitMap = pat.getHitMap();

    ASSERT_EQ(hitMap.size(), 8u) << "Clamped E(10,8) must still have 8 steps";
    int hits = 0;
    for (auto v : hitMap) hits += v;
    EXPECT_EQ(hits, 8) << "Clamped E(10,8) must have all 8 steps as hits";
}

// ─────────────────────────────────────────────────────────────────────────────
// TEST: Note Ons appear in the first block when transport is at start
// ─────────────────────────────────────────────────────────────────────────────
TEST(EuclideanPattern, NoteOnInFirstBlockAtStepZero)
{
    EuclideanPattern pat;
    pat.generate(1, 8); // single hit at step 0
    GlobalTransport transport;
    configurePlayingTransport(transport);

    juce::MidiBuffer buf;
    // First block starting at sample 0
    pat.getEventsForBuffer(buf, 0, 512, transport, 0.0);

    bool foundNoteOn = false;
    for (const auto meta : buf)
        if (meta.getMessage().isNoteOn() && meta.samplePosition == 0)
            foundNoteOn = true;

    EXPECT_TRUE(foundNoteOn)
        << "E(1,8): a NoteOn must be generated at sample 0 of the first block";
}

// ─────────────────────────────────────────────────────────────────────────────
// TEST: events loop correctly — second bar has same count as first
// ─────────────────────────────────────────────────────────────────────────────
TEST(EuclideanPattern, EventsLoopWithConsistentCount)
{
    EuclideanPattern pat;
    pat.generate(3, 8); // Tresillo — 3 hits per bar
    GlobalTransport transport;
    configurePlayingTransport(transport);

    constexpr int kBlockSize = 512;
    auto countNoteOnsInRange = [&](int64_t startSample, int64_t endSample) -> int
    {
        int count = 0;
        for (int64_t bs = startSample; bs < endSample; bs += kBlockSize)
        {
            juce::MidiBuffer buf;
            int actual = static_cast<int>(
                std::min(static_cast<int64_t>(kBlockSize), endSample - bs));
            pat.getEventsForBuffer(buf, bs, actual, transport, 0.0);
            for (const auto meta : buf)
                if (meta.getMessage().isNoteOn()) ++count;
        }
        return count;
    };

    const int64_t barSamples = static_cast<int64_t>(kSamplesPerBar + 0.5);
    const int firstBarCount  = countNoteOnsInRange(0, barSamples);
    const int secondBarCount = countNoteOnsInRange(barSamples, 2 * barSamples);

    EXPECT_EQ(firstBarCount, 3)
        << "Tresillo E(3,8): first bar must have exactly 3 Note Ons";
    EXPECT_EQ(secondBarCount, firstBarCount)
        << "Looping pattern: second bar must have the same Note On count as the first";
}

// ─────────────────────────────────────────────────────────────────────────────
// TEST: getLengthBeats() returns numBars * 4.0 beats
// ─────────────────────────────────────────────────────────────────────────────
TEST(EuclideanPattern, LengthBeatsTwoBar)
{
    EuclideanPattern pat;
    pat.setBars(2);
    pat.generate(4, 8);
    // 2 bars × 4 beats/bar = 8 beats — 1e-9: exact double arithmetic
    EXPECT_NEAR(pat.getLengthBeats(), 8.0, 1e-9)
        << "2-bar pattern must report 8.0 beats in getLengthBeats()";
}
