// SPDX-License-Identifier: AGPL-3.0-or-later
// LiBeDAW unit tests — Suite B: GlobalTransport
#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include "GlobalTransport.h"

// ─── Named constants ─────────────────────────────────────────────────────────
constexpr double kSampleRate     = 48000.0;
constexpr double kBpm120         = 120.0;
constexpr double kBpm60          = 60.0;
// At 120 BPM, 48 kHz → samplesPerBeat = 24000
constexpr double kSpb120         = kSampleRate * 60.0 / kBpm120;
// At 60 BPM, 48 kHz → samplesPerBeat = 48000
constexpr double kSpb60          = kSampleRate * 60.0 / kBpm60;
constexpr int    kAdvanceSamples = 512;

// ─────────────────────────────────────────────────────────────────────────────
// TEST: setBpm + setSampleRate → correct samplesPerBeat at 120 BPM
// ─────────────────────────────────────────────────────────────────────────────
TEST(GlobalTransport, BPM120_SPB_IsCorrect)
{
    GlobalTransport t;
    t.setSampleRate(kSampleRate);
    t.setBpm(kBpm120);
    // 1e-3: double arithmetic for 48000 * 60 / 120 = 24000.0 exactly
    EXPECT_NEAR(t.getSamplesPerBeat(), kSpb120, 1e-3)
        << "samplesPerBeat at 120 BPM / 48 kHz must equal 24000";
}

// ─────────────────────────────────────────────────────────────────────────────
// TEST: setBpm + setSampleRate → correct samplesPerBeat at 60 BPM
// ─────────────────────────────────────────────────────────────────────────────
TEST(GlobalTransport, BPM60_SPB_IsCorrect)
{
    GlobalTransport t;
    t.setSampleRate(kSampleRate);
    t.setBpm(kBpm60);
    // 1e-3: exact integer result for 48000 * 60 / 60 = 48000
    EXPECT_NEAR(t.getSamplesPerBeat(), kSpb60, 1e-3)
        << "samplesPerBeat at 60 BPM / 48 kHz must equal 48000";
}

// ─────────────────────────────────────────────────────────────────────────────
// TEST: play() then advanceBy() increments playhead position
// ─────────────────────────────────────────────────────────────────────────────
TEST(GlobalTransport, PlayAdvancesPlayheadPosition)
{
    GlobalTransport t;
    t.setSampleRate(kSampleRate);
    t.setBpm(kBpm120);
    t.play();
    t.advanceBy(kAdvanceSamples);

    EXPECT_EQ(t.getPlayheadPosition(), kAdvanceSamples)
        << "After play() + advanceBy(N), playhead must be at N";
}

// ─────────────────────────────────────────────────────────────────────────────
// TEST: advanceBy() while stopped does not move playhead
// ─────────────────────────────────────────────────────────────────────────────
TEST(GlobalTransport, StoppedTransportDoesNotAdvance)
{
    GlobalTransport t;
    t.setSampleRate(kSampleRate);
    t.setBpm(kBpm120);
    // Do NOT call t.play()
    t.advanceBy(kAdvanceSamples);

    EXPECT_EQ(t.getPlayheadPosition(), 0)
        << "advanceBy() while stopped must not change playhead position";
}

// ─────────────────────────────────────────────────────────────────────────────
// TEST: stop() resets playhead to 0 and clears recording flag
// ─────────────────────────────────────────────────────────────────────────────
TEST(GlobalTransport, StopResetsPlayheadAndRecording)
{
    GlobalTransport t;
    t.setSampleRate(kSampleRate);
    t.setBpm(kBpm120);
    t.play();
    t.toggleRecord();
    t.advanceBy(kAdvanceSamples * 10);
    t.stop();

    EXPECT_EQ(t.getPlayheadPosition(), 0)
        << "stop() must reset playhead to sample 0";
    EXPECT_FALSE(t.getIsRecording())
        << "stop() must clear the recording flag";
    EXPECT_FALSE(t.getIsPlaying())
        << "stop() must clear the isPlaying flag";
}

// ─────────────────────────────────────────────────────────────────────────────
// TEST: setLoop() / getLoop accessors round-trip correctly
// ─────────────────────────────────────────────────────────────────────────────
TEST(GlobalTransport, LoopBoundsRoundTrip)
{
    GlobalTransport t;
    t.setSampleRate(kSampleRate);
    t.setBpm(kBpm120);

    constexpr int64_t kLoopStart = 0;
    constexpr int64_t kLoopEnd   = 96000; // 2 bars @ 120 BPM / 48 kHz
    t.setLoop(kLoopStart, kLoopEnd);

    EXPECT_TRUE(t.getLoopEnabled())  << "setLoop() must enable the loop";
    EXPECT_EQ(t.getLoopStart(), kLoopStart) << "getLoopStart() must match setLoop() first arg";
    EXPECT_EQ(t.getLoopEnd(),   kLoopEnd)   << "getLoopEnd() must match setLoop() second arg";
}

// ─────────────────────────────────────────────────────────────────────────────
// TEST: clearLoop() disables the loop
// ─────────────────────────────────────────────────────────────────────────────
TEST(GlobalTransport, ClearLoopDisablesLoop)
{
    GlobalTransport t;
    t.setLoop(0, 96000);
    ASSERT_TRUE(t.getLoopEnabled()) << "Pre-condition: loop must be enabled after setLoop()";

    t.clearLoop();
    EXPECT_FALSE(t.getLoopEnabled()) << "clearLoop() must disable the loop";
}

// ─────────────────────────────────────────────────────────────────────────────
// TEST: getNextBarPosition() at sample 0 → returns samplesPerBar
// ─────────────────────────────────────────────────────────────────────────────
TEST(GlobalTransport, NextBarPositionAtStartIsOneBar)
{
    GlobalTransport t;
    t.setSampleRate(kSampleRate);
    t.setBpm(kBpm120);
    t.play();
    // Playhead is at 0

    const double spb      = t.getSamplesPerBeat();
    const double expected = spb * 4.0; // 4 beats/bar

    // 1.0 sample tolerance: ceil arithmetic may vary by 1 sample
    EXPECT_NEAR(t.getNextBarPosition(4), expected, 1.0)
        << "At sample 0, next bar must be exactly 1 bar (4 beats) ahead";
}

// ─────────────────────────────────────────────────────────────────────────────
// TEST: getNextBarPosition() at mid-bar → returns start of next bar
// ─────────────────────────────────────────────────────────────────────────────
TEST(GlobalTransport, NextBarPositionMidBar)
{
    GlobalTransport t;
    t.setSampleRate(kSampleRate);
    t.setBpm(kBpm120);
    t.play();

    const double spb        = t.getSamplesPerBeat();
    const double midBar     = spb * 2.0; // halfway through bar 1
    t.seekTo(static_cast<int64_t>(midBar));

    const double nextBar    = spb * 4.0; // start of bar 2 (from time 0)
    // 1.0 sample tolerance: ceil arithmetic
    EXPECT_NEAR(t.getNextBarPosition(4), nextBar, 1.0)
        << "At mid-bar, next bar position must be the start of bar 2";
}

// ─────────────────────────────────────────────────────────────────────────────
// TEST: seekTo() repositions playhead without altering play state
// ─────────────────────────────────────────────────────────────────────────────
TEST(GlobalTransport, SeekToRepositionsPlayhead)
{
    GlobalTransport t;
    t.setSampleRate(kSampleRate);
    t.setBpm(kBpm120);
    t.play();
    t.advanceBy(kAdvanceSamples);

    constexpr int64_t kTarget = 99999;
    t.seekTo(kTarget);

    EXPECT_EQ(t.getPlayheadPosition(), kTarget)
        << "seekTo() must set playhead to the exact target sample";
    EXPECT_TRUE(t.getIsPlaying())
        << "seekTo() must not change the play state";
}

// ─────────────────────────────────────────────────────────────────────────────
// TEST: toggleRecord() flips recording state
// ─────────────────────────────────────────────────────────────────────────────
TEST(GlobalTransport, ToggleRecordFlipsState)
{
    GlobalTransport t;
    ASSERT_FALSE(t.getIsRecording()) << "Pre-condition: transport must start not recording";

    t.toggleRecord();
    EXPECT_TRUE(t.getIsRecording())  << "First toggleRecord() must enable recording";

    t.toggleRecord();
    EXPECT_FALSE(t.getIsRecording()) << "Second toggleRecord() must disable recording";
}
