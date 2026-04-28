#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  ApcMiniDriver.h — Phase 5.2
//
//  Concrete ControlSurface driver for the Akai APC Mini (Mk1).
//
//  Hardware layout (Mk1):
//    • 8×8 clip grid     — Note On/Off, Ch.1, notes 0–63
//                          row 0 = notes 0–7,  row 1 = notes 8–15, …
//    • Scene buttons     — Ch.1, notes 82–89 (right column, top-to-bottom)
//    • Track buttons     — Ch.1, notes 64–71 (bottom row)
//    • Shift button      — Ch.1, note 98
//    • Faders (track)    — CC 48–55, Ch.1 (tracks 0–7)
//    • Master fader      — CC 56,    Ch.1
//
//  LED colours (Note On velocity sent to device):
//    0 = Off        1 = Green       2 = Green Blink
//    3 = Red        4 = Red Blink   5 = Yellow      6 = Yellow Blink
//
//  Anti-feedback guarantee:
//    Incoming MIDI from APC Mini → DAW callback path.
//    LED feedback → midiOut path.
//    These two paths are completely separate; no re-entrant LED sends
//    occur on the MIDI-in path.
//
//  All LED sends happen synchronously on the JUCE message thread via the
//  ClipStateObserver callbacks.  No auxiliary threads are created.
// ─────────────────────────────────────────────────────────────────────────────

#include <JuceHeader.h>
#include <array>
#include "ControlSurface.h"
#include "ControlElement.h"
#include "SessionComponent.h"
#include "MixerComponent.h"

class MainComponent;

// Forward-declare the ALSA rawmidi type so we don't need to pull in
// <alsa/asoundlib.h> in the header (which drags in a lot of system types).
typedef struct _snd_rawmidi snd_rawmidi_t;

class ApcMiniDriver : public ControlSurface,
                      private juce::Timer
{
public:
    explicit ApcMiniDriver (MainComponent& app);
    ~ApcMiniDriver() override;

    // ── ControlSurface interface ──────────────────────────────────────────────
    juce::String getDeviceName() const override { return "APC mini"; }
    bool         handleMidi    (const juce::MidiMessage& msg) override;
    void         fullSync      () override;

    // Called by ControlSurfaceManager when the JUCE output handle is assigned.
    // We use this moment to also open the rawmidi output for LED sends.
    void setMidiOutput (juce::MidiOutput* out) noexcept;

    // ── ClipStateObserver interface ───────────────────────────────────────────
    void onClipStateChanged (int trackIdx, int sceneIdx, ClipState state) override;
    void onTransportChanged (bool isPlaying, bool isRecording) override;
    void onLayoutChanged    () override;

private:
    void timerCallback() override;

    // ── APC Mini LED velocity mapping ─────────────────────────────────────────
    // Maps a ClipState to an APC Mini–specific velocity value.
    // Off=0, Green=1, GreenBlink=2, Red=3, RedBlink=4, Yellow=5, YellowBlink=6
    int stateToVelocity (ClipState state) const;

    // ── Hardware wiring ───────────────────────────────────────────────────────
    // Wires all elements to SessionComponent and MixerComponent callbacks.
    // Called once from the constructor.
    void buildMapping();

    // ── Hardware elements (stack-allocated, owned by driver) ─────────────────
    // gridButtons[track][scene] — Ch.1, note = scene * 8 + track
    std::array<std::array<ButtonElement, 8>, 8> gridButtons;
    //   gridButtons[t][s] is constructed with note = s * 8 + t  (see ctor)

    // Scene launch buttons — Ch.1, notes 82–89 (scene 0 = note 82, …)
    std::array<ButtonElement, 8> sceneButtons;

    // Track select / arm buttons — Ch.1, notes 64–71
    std::array<ButtonElement, 8> trackButtons;

    // Shift button — Ch.1, note 98
    ButtonElement shiftButton;

    // Faders: tracks 0–7 = CC 48–55, master = CC 56, all Ch.1
    std::array<SliderElement, 9> faders;

    // ── Logical components (not owners) ──────────────────────────────────────
    SessionComponent sessionComp;  // 8×8 session ring
    MixerComponent   mixerComp;    // 8 track + 1 master fader

    // ── State ─────────────────────────────────────────────────────────────────
    bool            shiftHeld  = false;
    snd_rawmidi_t*  rawMidiOut = nullptr;   // direct ALSA rawmidi for LED sends
    MainComponent&  app;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ApcMiniDriver)
};
