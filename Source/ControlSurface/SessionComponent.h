#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  SessionComponent.h
//
//  Logical component that maps an NxM "Session Ring" window over LiBeDAW's
//  infinite clip grid.  Hardware buttons are bound via setSlotButton().
//
//  The session ring can scroll horizontally (tracks) and vertically (scenes)
//  independently, and the LED state for every visible slot is updated via the
//  ClipStateObserver protocol — no polling.
//
//  Design notes:
//   • This class does NOT own ButtonElements — the concrete driver does.
//   • lastKnownState avoids redundant LED sends (only sends when state differs).
//   • onSlotPressed callback is wired by the driver to trigger clip launch in
//     MainComponent.
// ─────────────────────────────────────────────────────────────────────────────

#include <JuceHeader.h>
#include "ClipStateObserver.h"
#include "ControlElement.h"

class SessionComponent : public ClipStateObserver
{
public:
    // width / height: grid dimensions of the physical hardware (e.g. 8x8).
    SessionComponent (int width, int height);

    // ── Hardware binding ──────────────────────────────────────────────────────
    // Assign a physical button to grid coordinate (x=track, y=scene).
    // x and y are relative to the hardware grid (0-based).
    void setSlotButton (int x, int y, ButtonElement* btn);

    // ── Navigation ───────────────────────────────────────────────────────────
    void scrollTrack (int delta);   // shift ring left/right
    void scrollScene (int delta);   // shift ring up/down

    int getTrackOffset() const noexcept { return trackOffset; }
    int getSceneOffset() const noexcept { return sceneOffset; }

    // ── MIDI out ──────────────────────────────────────────────────────────────
    void setMidiOutput (juce::MidiOutput* out) noexcept { midiOut = out; }

    // Forces all slot LEDs to re-send based on lastKnownState.
    // Called on layout changes or device reconnect.
    void resync();

    // Invalidates all lastKnownState entries so the next onClipStateChanged call
    // always fires sendLed, even for Empty->Empty transitions.
    // Call this immediately before a full resync sweep to bypass the dedup guard.
    void invalidateLastKnownState() noexcept;

    // ── Callbacks wired by the concrete driver ────────────────────────────────
    // Called when the user presses a slot button (absolute trackIdx, sceneIdx).
    std::function<void (int trackIdx, int sceneIdx)> onSlotPressed;
    // Called when the session ring scrolls (so driver can update nav LEDs).
    std::function<void()> onRingMoved;

    // Optional mapping from state to hardware-specific LED velocity.
    std::function<int(ClipState)> stateMapper;

    // ── ClipStateObserver ─────────────────────────────────────────────────────
    void onClipStateChanged (int trackIdx, int sceneIdx, ClipState state) override;
    void onTransportChanged (bool isPlaying, bool isRecording) override;
    void onLayoutChanged() override;

private:
    // Convert from absolute session coordinates to ring-relative hardware coords.
    // Returns false if (absTrack, absScene) is outside the current ring window.
    bool toLocal (int absTrack, int absScene, int& localX, int& localY) const;

    // Push a single LED update for hardware coordinate (x, y).
    void sendLed (int x, int y, ClipState state);

    // Compute the LED color integer from state (delegates to driver mapping).
    // Default mapping provided here; drivers can call setStateMapper() to override.
    int defaultLedForState (ClipState state) const;

    int gridWidth;
    int gridHeight;
    int trackOffset = 0;
    int sceneOffset = 0;

    // [x][y] → ButtonElement pointer (not owned)
    std::vector<std::vector<ButtonElement*>> buttons;
    // [x][y] → last state sent to hardware (avoids redundant LED sends)
    std::vector<std::vector<ClipState>>      lastKnownState;

    juce::MidiOutput* midiOut = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SessionComponent)
};
