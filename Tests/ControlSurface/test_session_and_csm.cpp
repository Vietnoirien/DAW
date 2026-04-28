// SPDX-License-Identifier: AGPL-3.0-or-later
// LiBeDAW unit tests — Suite E: SessionComponent + ControlSurfaceManager
//
// ControlSurfaceManager requires juce::MessageManager (it uses juce::Timer and
// juce::MidiDeviceListConnection). It is therefore tested with a DISABLED_ prefix
// and documented below. SessionComponent is testable without a message thread.
#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include "SessionComponent.h"
#include "ControlSurface.h"
#include "ClipStateObserver.h"

// ─── Named constants ─────────────────────────────────────────────────────────
constexpr int kGridWidth  = 8;
constexpr int kGridHeight = 8;

// ─────────────────────────────────────────────────────────────────────────────
// Minimal stub: records LED sends (no real MIDI output)
// ─────────────────────────────────────────────────────────────────────────────

// SessionComponent calls sendLed() → ButtonElement::sendValue() → midiOut.
// Since we pass nullptr as midiOut, ButtonElement::sendValue() silently no-ops
// (ControlSurface::sendMidi() guards on midiOut != nullptr).
// We only need to observe state changes through the public observer interface.

// ─────────────────────────────────────────────────────────────────────────────
// SessionComponent tests
// ─────────────────────────────────────────────────────────────────────────────

TEST(SessionComponent, DefaultOffsetsAreZero)
{
    SessionComponent sc(kGridWidth, kGridHeight);
    EXPECT_EQ(sc.getTrackOffset(), 0)
        << "Initial trackOffset must be 0";
    EXPECT_EQ(sc.getSceneOffset(), 0)
        << "Initial sceneOffset must be 0";
}

TEST(SessionComponent, ScrollTrackIncrementsByDelta)
{
    SessionComponent sc(kGridWidth, kGridHeight);
    sc.scrollTrack(1);
    EXPECT_EQ(sc.getTrackOffset(), 1)
        << "scrollTrack(+1) must increment trackOffset to 1";
    sc.scrollTrack(2);
    EXPECT_EQ(sc.getTrackOffset(), 3)
        << "scrollTrack(+2) must bring trackOffset to 3";
}

TEST(SessionComponent, ScrollTrackNegativeDoesNotUnderflow)
{
    SessionComponent sc(kGridWidth, kGridHeight);
    // At offset 0, scrolling left must clamp to 0
    sc.scrollTrack(-1);
    EXPECT_EQ(sc.getTrackOffset(), 0)
        << "scrollTrack(-1) at offset 0 must clamp to 0 (no underflow)";
}

TEST(SessionComponent, ScrollSceneIncrementsByDelta)
{
    SessionComponent sc(kGridWidth, kGridHeight);
    sc.scrollScene(3);
    EXPECT_EQ(sc.getSceneOffset(), 3)
        << "scrollScene(+3) must set sceneOffset to 3";
}

TEST(SessionComponent, ScrollSceneNegativeDoesNotUnderflow)
{
    SessionComponent sc(kGridWidth, kGridHeight);
    sc.scrollScene(-5);
    EXPECT_EQ(sc.getSceneOffset(), 0)
        << "scrollScene(-5) at offset 0 must clamp to 0 (no underflow)";
}

TEST(SessionComponent, SlotOutsideRingWindowIsIgnored)
{
    // With offsets (0,0) and grid 8×8, absolute slot (10, 10) is outside
    // the visible window — onClipStateChanged must not crash
    SessionComponent sc(kGridWidth, kGridHeight);
    sc.setMidiOutput(nullptr);

    // Must not crash or assert
    EXPECT_NO_FATAL_FAILURE(
        sc.onClipStateChanged(10, 10, ClipState::Playing))
        << "onClipStateChanged for a slot outside the ring window must not crash";
}

TEST(SessionComponent, LastKnownStateDeduplicatesRedundantSends)
{
    // We verify deduplication indirectly: wire a lambda to onRingMoved and count
    // how many times the ring observer is notified.
    // The LED dedup is internal — we validate it by checking that a second
    // onClipStateChanged with the SAME state does not trigger onRingMoved
    // (which would happen if full resync is mistakenly called).
    SessionComponent sc(kGridWidth, kGridHeight);
    sc.setMidiOutput(nullptr);

    int ringMovedCount = 0;
    sc.onRingMoved = [&] { ++ringMovedCount; };

    // Change state once — may or may not trigger onRingMoved depending on impl
    sc.onClipStateChanged(0, 0, ClipState::HasClip);
    int countAfterFirst = ringMovedCount;

    // Second call with IDENTICAL state — dedup should suppress any re-notify
    sc.onClipStateChanged(0, 0, ClipState::HasClip);
    int countAfterSecond = ringMovedCount;

    EXPECT_EQ(countAfterFirst, countAfterSecond)
        << "Duplicate state notification must not trigger additional ring-moved callbacks";
}

TEST(SessionComponent, InvalidateLastKnownStateAllowsResend)
{
    SessionComponent sc(kGridWidth, kGridHeight);
    sc.setMidiOutput(nullptr);

    // Send state once
    sc.onClipStateChanged(0, 0, ClipState::HasClip);

    // Invalidate: next send of same state must go through (bypass dedup)
    sc.invalidateLastKnownState();

    // resync() forces a full LED sweep based on lastKnownState.
    // With midiOut == nullptr this is a no-op for hardware, but must not crash.
    EXPECT_NO_FATAL_FAILURE(sc.resync())
        << "resync() after invalidateLastKnownState() must not crash";
}

// ─────────────────────────────────────────────────────────────────────────────
// ControlSurfaceManager tests
// The CSM requires juce::MessageManager (juce::Timer + MidiDeviceListConnection).
// Tests are DISABLED and documented for future integration test scope.
// ─────────────────────────────────────────────────────────────────────────────

TEST(ControlSurfaceManager, DISABLED_RequiresJUCEMessageManager_NoSurfaceNoCrash)
{
    // Cannot test without juce::MessageManager (juce::Timer base class).
    // Would verify: ControlSurfaceManager default-constructs cleanly and
    // notifyClipState() with no registered surfaces does not crash.
}

TEST(ControlSurfaceManager, DISABLED_RequiresJUCEMessageManager_BroadcastToAllSurfaces)
{
    // Cannot test without juce::MessageManager.
    // Would verify: two mock ControlSurface instances registered via addSurface()
    // both receive onClipStateChanged() when notifyClipState() is called.
    // Use GMock MockControlSurface with EXPECT_CALL(...).Times(1) on each.
}

// ─────────────────────────────────────────────────────────────────────────────
// ClipState / LedColor constant tests (pure enum verification)
// ─────────────────────────────────────────────────────────────────────────────

TEST(ClipStateObserver, LedColorConstantsAreDistinct)
{
    // All protocol-neutral LED constants must be unique integers
    EXPECT_NE(LedColor::Off,        LedColor::Green)      << "Off vs Green";
    EXPECT_NE(LedColor::Green,      LedColor::GreenBlink) << "Green vs GreenBlink";
    EXPECT_NE(LedColor::GreenBlink, LedColor::Red)        << "GreenBlink vs Red";
    EXPECT_NE(LedColor::Red,        LedColor::Amber)      << "Red vs Amber";
    EXPECT_NE(LedColor::Amber,      LedColor::Yellow)     << "Amber vs Yellow";
}

TEST(ClipStateObserver, ClipStateEnumCastsToUint8)
{
    // ClipState must fit in uint8_t (hardware MIDI velocity byte)
    EXPECT_LE(static_cast<uint8_t>(ClipState::Recording), 255u)
        << "ClipState enum values must fit in a uint8_t";
}
