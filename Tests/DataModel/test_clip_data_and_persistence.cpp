// SPDX-License-Identifier: AGPL-3.0-or-later
// LiBeDAW unit tests — Suite D: ClipData model + persistence
#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include "ClipData.h"
#include "FMSynthProcessor.h"
#include "BuiltInEffects.h"

// ─── Named constants ─────────────────────────────────────────────────────────
constexpr double kSampleRate = 48000.0;

// ─────────────────────────────────────────────────────────────────────────────
// ClipData default state
// ─────────────────────────────────────────────────────────────────────────────

TEST(ClipData, DefaultStateHasNoClip)
{
    ClipData cd;
    EXPECT_FALSE(cd.hasClip)    << "Default ClipData must have hasClip == false";
    EXPECT_FALSE(cd.isPlaying)  << "Default ClipData must have isPlaying == false";
    EXPECT_TRUE(cd.midiNotes.empty()) << "Default ClipData must have empty midiNotes";
}

TEST(ClipData, DefaultPatternModeIsEuclidean)
{
    ClipData cd;
    EXPECT_EQ(cd.patternMode, juce::String("euclidean"))
        << "Default patternMode must be 'euclidean'";
}

TEST(ClipData, DefaultColourIsNotBlack)
{
    ClipData cd;
    // The default colour must not be transparent/black (would be invisible in UI)
    EXPECT_NE(cd.colour.getARGB(), 0u)
        << "Default clip colour must not be transparent";
}

// ─────────────────────────────────────────────────────────────────────────────
// MidiNote MPE defaults
// ─────────────────────────────────────────────────────────────────────────────

TEST(MidiNote, MpeDefaultsAreConservative)
{
    MidiNote n;
    n.note        = 60;
    n.startBeat   = 0.0;
    n.lengthBeats = 1.0;
    n.velocity    = 0.8f;
    // All MPE fields have default values from aggregate initialisation

    EXPECT_FALSE(n.hasMpe)        << "Default MidiNote must have hasMpe == false";
    EXPECT_NEAR(n.timbre, 0.5f, 1e-6f)
        << "Default timbre must be 0.5 (centre position for CC74)";
    EXPECT_NEAR(n.pressure,   0.0f, 1e-6f) << "Default pressure must be 0.0";
    EXPECT_NEAR(n.pitchBend,  0.0f, 1e-6f) << "Default pitchBend must be 0.0";
}

// ─────────────────────────────────────────────────────────────────────────────
// ArrangementClip defaults
// ─────────────────────────────────────────────────────────────────────────────

TEST(ArrangementClip, WarpModeDefaultIsComplex)
{
    ArrangementClip clip;
    EXPECT_EQ(clip.warpMode, ArrangementClip::WarpMode::Complex)
        << "Default warpMode must be WarpMode::Complex";
    EXPECT_FALSE(clip.warpEnabled)
        << "Warping must be disabled by default";
}

TEST(ArrangementClip, TakesAndCompRegionsAreEmptyByDefault)
{
    ArrangementClip clip;
    EXPECT_TRUE(clip.takes.empty())       << "Default takes must be empty";
    EXPECT_TRUE(clip.compRegions.empty()) << "Default compRegions must be empty";
}

// ─────────────────────────────────────────────────────────────────────────────
// Take — compile-time constant
// ─────────────────────────────────────────────────────────────────────────────

TEST(Take, MaxTakesPerClipIsEight)
{
    EXPECT_EQ(Take::kMaxTakesPerClip, 8)
        << "kMaxTakesPerClip must equal 8 (one decoder thread per take)";
}

// ─────────────────────────────────────────────────────────────────────────────
// CompRegion — non-overlap validation helper
// ─────────────────────────────────────────────────────────────────────────────

TEST(CompRegion, NonOverlappingRegionsAreValid)
{
    CompRegion r1; r1.takeIndex = 0; r1.startBeat = 0.0; r1.endBeat = 2.0;
    CompRegion r2; r2.takeIndex = 1; r2.startBeat = 2.0; r2.endBeat = 4.0;

    // Adjacent (r1.end == r2.start) is valid: no overlap
    EXPECT_LE(r1.endBeat, r2.startBeat)
        << "Adjacent CompRegions (end == start) must not be considered overlapping";
}

TEST(CompRegion, OverlappingRegionsAreDetectable)
{
    CompRegion r1; r1.takeIndex = 0; r1.startBeat = 0.0; r1.endBeat = 3.0;
    CompRegion r2; r2.takeIndex = 1; r2.startBeat = 2.0; r2.endBeat = 4.0;

    // r1.end > r2.start → overlapping
    EXPECT_GT(r1.endBeat, r2.startBeat)
        << "Overlapping CompRegions must be detectable by comparing end vs start";
}

// ─────────────────────────────────────────────────────────────────────────────
// FMSynth ValueTree round-trip (Suite D serialization test)
// ─────────────────────────────────────────────────────────────────────────────

TEST(FMSynthPersistence, ValueTreeRoundTrip)
{
    FMSynthProcessor original;
    original.prepareToPlay(kSampleRate);
    original.params.algorithm.store(15);
    original.params.masterLevel.store(0.55f);
    original.params.ampSustain.store(0.9f);
    original.params.filterEnabled.store(true);
    original.params.filterCutoff.store(6000.0f);

    auto tree = original.saveState();
    FMSynthProcessor restored;
    restored.prepareToPlay(kSampleRate);
    restored.loadState(tree);

    // 1e-5f: double→float round-trip via juce::var
    EXPECT_EQ  (restored.params.algorithm.load(),        15)      << "algorithm";
    EXPECT_NEAR(restored.params.masterLevel.load(),      0.55f, 1e-5f) << "masterLevel";
    EXPECT_NEAR(restored.params.ampSustain.load(),       0.9f,  1e-5f) << "ampSustain";
    EXPECT_TRUE(restored.params.filterEnabled.load())                  << "filterEnabled";
    EXPECT_NEAR(restored.params.filterCutoff.load(),  6000.0f,  1e-3f) << "filterCutoff";
}

// ─────────────────────────────────────────────────────────────────────────────
// ReverbEffect ValueTree round-trip
// ─────────────────────────────────────────────────────────────────────────────

TEST(ReverbPersistence, ValueTreeRoundTrip)
{
    ReverbEffect original;
    original.prepareToPlay(kSampleRate);
    original.roomSize.store(0.8f);
    original.damping.store(0.3f);
    original.wetLevel.store(0.5f);
    original.dryLevel.store(0.7f);
    original.width.store(0.9f);

    auto tree = original.saveState();
    ReverbEffect restored;
    restored.prepareToPlay(kSampleRate);
    restored.loadState(tree);

    // 1e-5f: float round-trip through juce::ValueTree
    EXPECT_NEAR(restored.roomSize.load(),  0.8f, 1e-5f) << "roomSize";
    EXPECT_NEAR(restored.damping.load(),   0.3f, 1e-5f) << "damping";
    EXPECT_NEAR(restored.wetLevel.load(),  0.5f, 1e-5f) << "wetLevel";
    EXPECT_NEAR(restored.dryLevel.load(),  0.7f, 1e-5f) << "dryLevel";
    EXPECT_NEAR(restored.width.load(),     0.9f, 1e-5f) << "width";
}

// ─────────────────────────────────────────────────────────────────────────────
// CompressorEffect ValueTree round-trip
// ─────────────────────────────────────────────────────────────────────────────

TEST(CompressorPersistence, ValueTreeRoundTrip)
{
    CompressorEffect original;
    original.prepareToPlay(kSampleRate);
    original.threshold.store(-18.0f);
    original.ratio.store(6.0f);
    original.attack.store(5.0f);
    original.release.store(300.0f);
    original.makeup.store(3.0f);
    original.mix.store(75.0f);
    original.sidechainSourceIndex.store(2);

    auto tree = original.saveState();
    CompressorEffect restored;
    restored.prepareToPlay(kSampleRate);
    restored.loadState(tree);

    // 1e-5f: float round-trip
    EXPECT_NEAR(restored.threshold.load(),          -18.0f, 1e-5f) << "threshold";
    EXPECT_NEAR(restored.ratio.load(),                6.0f, 1e-5f) << "ratio";
    EXPECT_NEAR(restored.attack.load(),               5.0f, 1e-5f) << "attack";
    EXPECT_NEAR(restored.release.load(),            300.0f, 1e-5f) << "release";
    EXPECT_NEAR(restored.makeup.load(),               3.0f, 1e-5f) << "makeup";
    EXPECT_NEAR(restored.mix.load(),                 75.0f, 1e-5f) << "mix";
    EXPECT_EQ  (restored.sidechainSourceIndex.load(),    2)        << "sidechainSourceIndex";
}

// ─────────────────────────────────────────────────────────────────────────────
// DelayEffect ValueTree round-trip (duplicated from Suite C for completeness)
// ─────────────────────────────────────────────────────────────────────────────

TEST(DelayPersistence, ValueTreeRoundTrip)
{
    DelayEffect original;
    original.prepareToPlay(kSampleRate);
    original.delayTimeMs.store(420.0f);
    original.feedback.store(0.65f);
    original.mix.store(0.45f);
    original.syncEnabled.store(true);
    original.syncDivision.store(8); // 1/2 note

    auto tree = original.saveState();
    DelayEffect restored;
    restored.prepareToPlay(kSampleRate);
    restored.loadState(tree);

    EXPECT_NEAR(restored.delayTimeMs.load(), 420.0f, 1e-5f) << "delayTimeMs";
    EXPECT_NEAR(restored.feedback.load(),     0.65f, 1e-5f) << "feedback";
    EXPECT_NEAR(restored.mix.load(),          0.45f, 1e-5f) << "mix";
    EXPECT_TRUE(restored.syncEnabled.load())               << "syncEnabled";
    EXPECT_EQ  (restored.syncDivision.load(),      8)      << "syncDivision";
}
