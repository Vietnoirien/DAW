#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  ControlSurfaceManager.h
//
//  Owned by MainComponent.
//  Responsibilities:
//    1. Holds all registered ControlSurface instances.
//    2. Auto-detects and opens MIDI output for each surface on registration.
//    3. Routes incoming MIDI messages to the first surface that consumes them.
//    4. Broadcasts state changes (clip state, transport, layout) to all surfaces.
//
//  Thread safety:
//    All public methods MUST be called from the JUCE message thread only.
//    (handleMidi is called from handleIncomingMidiMessage which JUCE guarantees
//    to call on the message thread for MidiInputCallback.)
// ─────────────────────────────────────────────────────────────────────────────

#include <JuceHeader.h>
#include "ControlSurface.h"

class ControlSurfaceManager : private juce::Timer
{
public:
    ControlSurfaceManager()
    {
        // Re-check output connections whenever the MIDI device list changes.
        // This fires when JUCE's AudioDeviceManager enables a MIDI input on the
        // same physical device we have open as an output, which can mark our
        // existing output connection as "not alive".  Re-opening it here ensures
        // we always hold a live handle before the first LED send.
        deviceListConnection = juce::MidiDeviceListConnection::make ([this]
        {
            // Only try to open currently-null handles.  Do NOT force-reopen
            // a live handle: opening a new MidiOutput itself triggers another
            // device-list-change, causing an infinite reconnect loop.
            juce::MessageManager::callAsync ([this] { refreshOutputs (/*forceReopen=*/false); });
        });
    }

    ~ControlSurfaceManager() override { stopTimer(); }

    // ── Surface registration ──────────────────────────────────────────────────
    // Adds a surface, auto-detects its MIDI output, and calls fullSync().
    void addSurface (std::unique_ptr<ControlSurface> surface);

    // ── MIDI routing ──────────────────────────────────────────────────────────
    // Call from MainComponent::handleIncomingMidiMessage BEFORE existing logic.
    // Returns true if the message was consumed by a surface (do not process further).
    bool handleMidi (const juce::MidiMessage& msg);

    // ── State broadcast ───────────────────────────────────────────────────────
    // Call these at the exact moment DAW state changes (message thread only).
    void notifyClipState  (int trackIdx, int sceneIdx, ClipState state);
    void notifyTransport  (bool isPlaying, bool isRecording);
    void notifyLayout();   // tracks added / removed

    // Disconnects all hardware safely
    void removeAllSurfaces()
    {
        surfaces.clear();
        midiOutputs.clear();
    }

private:
    void timerCallback() override;

    // Scan juce::MidiOutput::getAvailableDevices() for a name matching the
    // surface's getDeviceName() substring (case-insensitive).
    // Returns nullptr if no match is found.
    std::unique_ptr<juce::MidiOutput> findOutputForSurface (const ControlSurface& surface);

    // Re-open MIDI outputs.
    // forceReopen=true  — drop and re-open ALL handles (device-list-change path).
    // forceReopen=false — only open currently-null handles (timer polling path).
    void refreshOutputs (bool forceReopen = true);

    std::vector<std::unique_ptr<juce::MidiOutput>> midiOutputs; // must outlive surfaces
    std::vector<std::unique_ptr<ControlSurface>>   surfaces;

    // Keeps our reconnect lambda alive for as long as this manager exists.
    juce::MidiDeviceListConnection deviceListConnection;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ControlSurfaceManager)
};
