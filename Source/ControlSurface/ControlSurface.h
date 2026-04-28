#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  ControlSurface.h
//
//  Abstract base class for every hardware controller driver.
//  Concrete drivers (e.g. ApcMiniDriver) inherit from this and:
//    • Declare getDeviceName() → used by ControlSurfaceManager for auto-detect.
//    • Implement handleMidi()  → consume messages meant for this surface.
//    • Override ClipStateObserver callbacks → push LED feedback to hardware.
//
//  The midiOut pointer is set by ControlSurfaceManager after auto-detection.
//  sendMidi() is a convenience wrapper that forwards to midiOut safely.
// ─────────────────────────────────────────────────────────────────────────────

#include <JuceHeader.h>
#include "ClipStateObserver.h"

class ControlSurface : public ClipStateObserver
{
public:
    ~ControlSurface() override = default;

    // Returns the device name substring used for MIDI I/O auto-detection.
    // Example: "APC mini" will match both "APC mini" input and output ports.
    virtual juce::String getDeviceName() const = 0;

    // Called by ControlSurfaceManager::handleMidi().
    // Returns true if the message was consumed by this surface.
    virtual bool handleMidi (const juce::MidiMessage& msg) = 0;

    // Called once by ControlSurfaceManager after the matching MIDI output port
    // has been opened.  May be called again with nullptr if the device disconnects.
    void setMidiOutput (juce::MidiOutput* out) noexcept { midiOut = out; }

    // Performs a full LED resync to match current DAW state.
    // Called after setMidiOutput() and after onLayoutChanged().
    virtual void fullSync() {}

protected:
    // Convenience: send a MIDI message to the hardware.
    // Thread-safe when called from the message thread only.
    void sendMidi (const juce::MidiMessage& msg)
    {
        if (midiOut != nullptr)
            midiOut->sendMessageNow (msg);
    }

    juce::MidiOutput* midiOut = nullptr;
};
