#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  ClipStateObserver.h
//
//  Observer interface for clip / transport state changes.
//  MainComponent calls the notify* methods synchronously on the message thread
//  whenever clip state or transport state changes.  Every ControlSurface
//  inherits this interface and converts the state into protocol-specific MIDI
//  LED messages for its hardware.
//
//  LedColor provides default velocity/value constants.  Hardware drivers
//  (e.g. ApcMiniDriver) override mapStateToLed() to use device-specific values.
// ─────────────────────────────────────────────────────────────────────────────

#include <JuceHeader.h>

// ── State representation ──────────────────────────────────────────────────────
enum class ClipState : uint8_t
{
    Empty,      // No clip in this slot
    HasClip,    // Clip exists, not playing  → Amber/Yellow LED
    Queued,     // Clip queued for launch    → Green blink LED
    Playing,    // Clip actively playing     → Green solid LED
    Recording   // Track armed + recording   → Red LED
};

// ── LED color vocabulary (protocol-neutral integer values) ────────────────────
// Hardware drivers remap these to their own velocity / CC values.
struct LedColor
{
    static constexpr int Off        = 0;
    static constexpr int Green      = 1;   // Playing
    static constexpr int GreenBlink = 2;   // Queued (pulsing)
    static constexpr int Red        = 3;   // Recording / armed
    static constexpr int Amber      = 5;   // HasClip, stopped
    static constexpr int Yellow     = 6;   // Scene active
};

// ── Observer interface ────────────────────────────────────────────────────────
class ClipStateObserver
{
public:
    virtual ~ClipStateObserver() = default;

    // Called when a single clip slot changes state.
    // trackIdx / sceneIdx are 0-based indices into the Session grid.
    virtual void onClipStateChanged (int trackIdx, int sceneIdx, ClipState state) = 0;

    // Called when the global transport play/record state changes.
    virtual void onTransportChanged (bool isPlaying, bool isRecording) = 0;

    // Called when tracks are added or removed — surface should perform a full
    // LED resync because grid geometry has changed.
    virtual void onLayoutChanged() = 0;
};
