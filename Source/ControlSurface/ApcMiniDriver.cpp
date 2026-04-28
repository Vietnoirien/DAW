// ─────────────────────────────────────────────────────────────────────────────
//  ApcMiniDriver.cpp — Phase 5.2
//
//  Concrete driver for the Akai APC Mini Mk1.
//  See ApcMiniDriver.h for hardware protocol documentation.
// ─────────────────────────────────────────────────────────────────────────────

#include "ApcMiniDriver.h"
#include <cstdio>  // fprintf
#include <alsa/asoundlib.h>
#include "MainComponent.h"

// ── APC Mini Mk1 MIDI note / CC constants ────────────────────────────────────
namespace ApcMini
{
    static constexpr int kChannel      = 1;   // All messages on Ch.1

    // Grid: note = sceneRow * 8 + trackCol  (sceneRow 0=top, trackCol 0=left)
    static constexpr int kGridBase     = 0;   // note 0 (track 0, scene 0)

    // Bottom-row track buttons (select / arm)
    static constexpr int kTrackBase    = 64;  // notes 64–71

    // Right-column scene launch buttons
    static constexpr int kSceneBase    = 82;  // notes 82–89

    // Shift button
    static constexpr int kShift        = 98;

    // Faders: CC 48–55 = tracks 0–7, CC 56 = master
    static constexpr int kFaderBase    = 48;
    static constexpr int kMasterFader  = 56;

    // LED velocities
    static constexpr int kOff          = 0;
    static constexpr int kGreen        = 1;
    static constexpr int kGreenBlink   = 2;
    static constexpr int kRed          = 3;
    static constexpr int kRedBlink     = 4;
    static constexpr int kYellow       = 5;
    static constexpr int kYellowBlink  = 6;
}

// ─────────────────────────────────────────────────────────────────────────────
//  openRawMidiOutput — find and open the APC Mini rawmidi output device
//
//  JUCE routes MIDI output through the ALSA sequencer, which normalises
//  NoteOn-vel-0 → NoteOff inside snd_midi_event_encode (alsa-lib source:
//    ev->type = c[1] ? SND_SEQ_EVENT_NOTEON : SND_SEQ_EVENT_NOTEOFF;
//  ).  The re-encoded byte on the wire is therefore 0x80 (NoteOff), which
//  the APC Mini ignores for LED purposes.
//
//  snd_rawmidi_write() sends bytes verbatim without any normalization, so
//  [0x90, note, 0x00] (NoteOn vel=0) actually reaches the hardware.
// ─────────────────────────────────────────────────────────────────────────────
static snd_rawmidi_t* openRawMidiOutput (const juce::String& surfaceName)
{
    // Iterate every ALSA sound card looking for a rawmidi output device
    // whose name contains the surface's device-name substring.
    int card = -1;
    while (snd_card_next (&card) == 0 && card >= 0)
    {
        char ctlName[16];
        snprintf (ctlName, sizeof (ctlName), "hw:%d", card);
        snd_ctl_t* ctl = nullptr;
        if (snd_ctl_open (&ctl, ctlName, 0) < 0) continue;

        int dev = -1;
        while (snd_ctl_rawmidi_next_device (ctl, &dev) == 0 && dev >= 0)
        {
            snd_rawmidi_info_t* info;
            snd_rawmidi_info_alloca (&info);
            snd_rawmidi_info_set_device    (info, dev);
            snd_rawmidi_info_set_stream    (info, SND_RAWMIDI_STREAM_OUTPUT);
            snd_rawmidi_info_set_subdevice (info, 0);
            if (snd_ctl_rawmidi_info (ctl, info) < 0) continue;

            const juce::String devName { snd_rawmidi_info_get_name (info) };
            if (devName.containsIgnoreCase (surfaceName))
            {
                char path[32];
                snprintf (path, sizeof (path), "hw:%d,%d", card, dev);
                snd_rawmidi_t* handle = nullptr;
                if (snd_rawmidi_open (nullptr, &handle, path, 0) == 0)
                {
                    fprintf (stderr, "[APC] rawmidi output opened: %s (%s)\n",
                             path, devName.toRawUTF8());
                    snd_ctl_close (ctl);
                    return handle;
                }
            }
        }
        snd_ctl_close (ctl);
    }
    fprintf (stderr, "[APC] rawmidi output not found for '%s'\n",
             surfaceName.toRawUTF8());
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
//  sendLedRaw — write a raw NoteOn packet directly to the ALSA rawmidi device
// ─────────────────────────────────────────────────────────────────────────────
static void sendLedRaw (snd_rawmidi_t* raw, int channel, int note, int velocity)
{
    if (raw == nullptr) return;
    const unsigned char data[3] = {
        (unsigned char) (0x8F + channel),   // 0x90 for ch1
        (unsigned char) (note     & 0x7F),
        (unsigned char) (velocity & 0x7F)
    };
    snd_rawmidi_write (raw, data, 3);
    // NOTE: do NOT drain after every write — batch writes are more efficient
    // and a single snd_rawmidi_drain at the end of a sweep is sufficient.
}

// ─────────────────────────────────────────────────────────────────────────────
//  Constructor
//
//  Initialises all hardware elements with correct MIDI note / CC numbers, then
//  wires them to SessionComponent and MixerComponent.
//
//  ButtonElement / SliderElement members in std::array are default-constructed
//  by the compiler's aggregate init; we use a delegating member-initialiser list
//  to give each element its correct MIDI mapping before buildMapping() runs.
//
//  Because std::array<T,N> requires T to be default-constructible for aggregate
//  init, and ButtonElement / SliderElement each have a required-param ctor,
//  we explicitly initialise every element via placement using the array syntax
//  inside the member-initialiser list.
// ─────────────────────────────────────────────────────────────────────────────
ApcMiniDriver::ApcMiniDriver (MainComponent& appRef)
    // ── 8×8 grid buttons ─────────────────────────────────────────────────────
    // gridButtons[track][scene] — note = (7 - scene) * 8 + track
    : gridButtons {{
        // track 0
        {{ ButtonElement(ApcMini::kChannel, 56), ButtonElement(ApcMini::kChannel, 48),
           ButtonElement(ApcMini::kChannel, 40), ButtonElement(ApcMini::kChannel, 32),
           ButtonElement(ApcMini::kChannel, 24), ButtonElement(ApcMini::kChannel, 16),
           ButtonElement(ApcMini::kChannel,  8), ButtonElement(ApcMini::kChannel,  0) }},
        // track 1
        {{ ButtonElement(ApcMini::kChannel, 57), ButtonElement(ApcMini::kChannel, 49),
           ButtonElement(ApcMini::kChannel, 41), ButtonElement(ApcMini::kChannel, 33),
           ButtonElement(ApcMini::kChannel, 25), ButtonElement(ApcMini::kChannel, 17),
           ButtonElement(ApcMini::kChannel,  9), ButtonElement(ApcMini::kChannel,  1) }},
        // track 2
        {{ ButtonElement(ApcMini::kChannel, 58), ButtonElement(ApcMini::kChannel, 50),
           ButtonElement(ApcMini::kChannel, 42), ButtonElement(ApcMini::kChannel, 34),
           ButtonElement(ApcMini::kChannel, 26), ButtonElement(ApcMini::kChannel, 18),
           ButtonElement(ApcMini::kChannel, 10), ButtonElement(ApcMini::kChannel,  2) }},
        // track 3
        {{ ButtonElement(ApcMini::kChannel, 59), ButtonElement(ApcMini::kChannel, 51),
           ButtonElement(ApcMini::kChannel, 43), ButtonElement(ApcMini::kChannel, 35),
           ButtonElement(ApcMini::kChannel, 27), ButtonElement(ApcMini::kChannel, 19),
           ButtonElement(ApcMini::kChannel, 11), ButtonElement(ApcMini::kChannel,  3) }},
        // track 4
        {{ ButtonElement(ApcMini::kChannel, 60), ButtonElement(ApcMini::kChannel, 52),
           ButtonElement(ApcMini::kChannel, 44), ButtonElement(ApcMini::kChannel, 36),
           ButtonElement(ApcMini::kChannel, 28), ButtonElement(ApcMini::kChannel, 20),
           ButtonElement(ApcMini::kChannel, 12), ButtonElement(ApcMini::kChannel,  4) }},
        // track 5
        {{ ButtonElement(ApcMini::kChannel, 61), ButtonElement(ApcMini::kChannel, 53),
           ButtonElement(ApcMini::kChannel, 45), ButtonElement(ApcMini::kChannel, 37),
           ButtonElement(ApcMini::kChannel, 29), ButtonElement(ApcMini::kChannel, 21),
           ButtonElement(ApcMini::kChannel, 13), ButtonElement(ApcMini::kChannel,  5) }},
        // track 6
        {{ ButtonElement(ApcMini::kChannel, 62), ButtonElement(ApcMini::kChannel, 54),
           ButtonElement(ApcMini::kChannel, 46), ButtonElement(ApcMini::kChannel, 38),
           ButtonElement(ApcMini::kChannel, 30), ButtonElement(ApcMini::kChannel, 22),
           ButtonElement(ApcMini::kChannel, 14), ButtonElement(ApcMini::kChannel,  6) }},
        // track 7
        {{ ButtonElement(ApcMini::kChannel, 63), ButtonElement(ApcMini::kChannel, 55),
           ButtonElement(ApcMini::kChannel, 47), ButtonElement(ApcMini::kChannel, 39),
           ButtonElement(ApcMini::kChannel, 31), ButtonElement(ApcMini::kChannel, 23),
           ButtonElement(ApcMini::kChannel, 15), ButtonElement(ApcMini::kChannel,  7) }}
    }}
    // ── Scene launch buttons (notes 82–89) ────────────────────────────────────
    , sceneButtons {{
        ButtonElement(ApcMini::kChannel, 82),
        ButtonElement(ApcMini::kChannel, 83),
        ButtonElement(ApcMini::kChannel, 84),
        ButtonElement(ApcMini::kChannel, 85),
        ButtonElement(ApcMini::kChannel, 86),
        ButtonElement(ApcMini::kChannel, 87),
        ButtonElement(ApcMini::kChannel, 88),
        ButtonElement(ApcMini::kChannel, 89)
    }}
    // ── Track select / arm buttons (notes 64–71) ─────────────────────────────
    , trackButtons {{
        ButtonElement(ApcMini::kChannel, 64),
        ButtonElement(ApcMini::kChannel, 65),
        ButtonElement(ApcMini::kChannel, 66),
        ButtonElement(ApcMini::kChannel, 67),
        ButtonElement(ApcMini::kChannel, 68),
        ButtonElement(ApcMini::kChannel, 69),
        ButtonElement(ApcMini::kChannel, 70),
        ButtonElement(ApcMini::kChannel, 71)
    }}
    // ── Shift button ──────────────────────────────────────────────────────────
    , shiftButton (ApcMini::kChannel, ApcMini::kShift)
    // ── Faders: CC 48–55 for tracks, CC 56 for master ────────────────────────
    , faders {{
        SliderElement(ApcMini::kChannel, 48),
        SliderElement(ApcMini::kChannel, 49),
        SliderElement(ApcMini::kChannel, 50),
        SliderElement(ApcMini::kChannel, 51),
        SliderElement(ApcMini::kChannel, 52),
        SliderElement(ApcMini::kChannel, 53),
        SliderElement(ApcMini::kChannel, 54),
        SliderElement(ApcMini::kChannel, 55),
        SliderElement(ApcMini::kChannel, 56)  // master
    }}
    // ── Logical components ────────────────────────────────────────────────────
    , sessionComp (8, 8)
    , mixerComp   (8)
    , app (appRef)
{
    buildMapping();
}

ApcMiniDriver::~ApcMiniDriver()
{
    // Stop the debounce timer first so no timerCallback fires during teardown.
    stopTimer();

    if (rawMidiOut != nullptr)
    {
        // Blank all grid and side LEDs (notes 0–98) in one batch sweep.
        // snd_rawmidi_drain flushes the kernel ring buffer to the USB host
        // controller; juce::Thread::sleep gives the USB HCI time to deliver
        // the packets to the device before we close the handle.
        for (int i = 0; i <= 98; ++i)
            sendLedRaw (rawMidiOut, ApcMini::kChannel, i, 0);

        snd_rawmidi_drain (rawMidiOut);
        juce::Thread::sleep (150); // wait for USB HCI to deliver all packets
        snd_rawmidi_close (rawMidiOut);
        rawMidiOut = nullptr;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  setMidiOutput — open rawmidi alongside the JUCE output handle
// ─────────────────────────────────────────────────────────────────────────────
void ApcMiniDriver::setMidiOutput (juce::MidiOutput* out) noexcept
{
    // Close any existing rawmidi handle first.
    if (rawMidiOut != nullptr)
    {
        snd_rawmidi_close (rawMidiOut);
        rawMidiOut = nullptr;
    }

    // Let the base class store the JUCE handle (used by ControlSurfaceManager
    // for MIDI input routing; we don't call sendMessageNow for LEDs).
    midiOut = out;
    sessionComp.setMidiOutput (out);

    // If a live JUCE output was just assigned, open the corresponding rawmidi.
    if (out != nullptr)
        rawMidiOut = openRawMidiOutput (getDeviceName());
}

// ─────────────────────────────────────────────────────────────────────────────
//  buildMapping — wires elements to SessionComponent / MixerComponent
// ─────────────────────────────────────────────────────────────────────────────
void ApcMiniDriver::buildMapping()
{
    // ── 1. Session ring: 8×8 clip grid ───────────────────────────────────────
    // setSlotButton(x=track, y=scene, btn) automatically wires onPress to
    // call sessionComp.onSlotPressed(absTrack, absScene).
    for (int t = 0; t < 8; ++t)
        for (int s = 0; s < 8; ++s)
            sessionComp.setSlotButton(t, s, &gridButtons[t][s]);

    // Wire the slot-pressed callback → MainComponent launch path
    sessionComp.onSlotPressed = [this] (int absTrack, int absScene)
    {
        app.csLaunchClip (absTrack, absScene);
    };

    // ── 2. Scene launch buttons (right column) ────────────────────────────────
    for (int s = 0; s < 8; ++s)
    {
        sceneButtons[s].onPress = [this, s] (int velocity)
        {
            if (velocity == 0) return; // ignore Note Off
            // Absolute scene index = sessionComp.getSceneOffset() + s
            int absScene = sessionComp.getSceneOffset() + s;
            app.csLaunchScene (absScene);
        };
    }

    // ── 3. Track select / arm buttons (bottom row) ────────────────────────────
    for (int t = 0; t < 8; ++t)
    {
        trackButtons[t].onPress = [this, t] (int velocity)
        {
            if (velocity == 0) return; // ignore Note Off

            int absTrack = sessionComp.getTrackOffset() + t;

            if (shiftHeld)
            {
                // Shift + Track button → stop the playing clip in this track
                int numScenes = app.getNumScenes (absTrack);
                for (int sc = 0; sc < numScenes; ++sc)
                {
                    if (app.getClipState (absTrack, sc) == ClipState::Playing)
                    {
                        app.csPauseClip (absTrack, sc);
                        break;
                    }
                }
            }
            else
            {
                // No shift → select track
                app.csSelectTrack (absTrack);
            }
        };
    }

    // ── 4. Shift button ───────────────────────────────────────────────────────
    shiftButton.onPress = [this] (int velocity)
    {
        shiftHeld = (velocity > 0);
    };

    // ── 5. Faders → MixerComponent ────────────────────────────────────────────
    // Tracks 0–7 (CC 48–55)
    for (int t = 0; t < 8; ++t)
        mixerComp.setVolumeSlider (t, &faders[t]);

    // Master fader (CC 56 = faders[8])
    mixerComp.setMasterSlider (&faders[8]);

    // Wire MixerComponent callbacks → MainComponent proxy methods
    mixerComp.onVolumeChanged = [this] (int trackIdx, float gain)
    {
        app.csTrackVolumeChanged (trackIdx, gain);
    };

    mixerComp.onMasterVolumeChanged = [this] (float gain)
    {
        app.csMasterVolumeChanged (gain);
    };

    // Arm callback wired through MixerComponent
    mixerComp.onArmChanged = [this] (int trackIdx, bool armed)
    {
        app.csTrackArmChanged (trackIdx, armed);
    };

    // ── 6. Share the MIDI output with sub-components ──────────────────────────
    sessionComp.setMidiOutput (midiOut);

    // ── 7. Configure LED state mapping for APC Mini velocities ────────────────
    sessionComp.stateMapper = [this] (ClipState state) {
        return stateToVelocity (state);
    };
}

// ─────────────────────────────────────────────────────────────────────────────
//  handleMidi — route incoming MIDI to matching element
// ─────────────────────────────────────────────────────────────────────────────
bool ApcMiniDriver::handleMidi (const juce::MidiMessage& msg)
{
    // Only process messages originating from the APC Mini (channel 1).
    // Note: ButtonElement / SliderElement also verify channel internally.
    if (!msg.isForChannel (ApcMini::kChannel))
        return false;

    // ── Anti-echo guard ───────────────────────────────────────────────────────
    // The APC Mini Mk1 echoes Note On messages that we send to it for LED
    // control back through its MIDI OUT port.  LED velocities are 1–6;
    // real button presses always arrive at velocity 127.
    // Drop Note Ons in the LED velocity range to break the feedback loop.
    if (msg.isNoteOn() && msg.getVelocity() >= 1 && msg.getVelocity() <= 6)
        return true; // consumed (silently ignored)

    // ── Shift button (check first so shiftHeld is up to date) ────────────────
    if (shiftButton.processMidi (msg)) return true;

    // ── 8×8 grid ──────────────────────────────────────────────────────────────
    for (auto& trackRow : gridButtons)
        for (auto& btn : trackRow)
            if (btn.processMidi (msg)) return true;

    // ── Scene launch buttons ──────────────────────────────────────────────────
    for (auto& btn : sceneButtons)
        if (btn.processMidi (msg)) return true;

    // ── Track buttons ─────────────────────────────────────────────────────────
    for (auto& btn : trackButtons)
        if (btn.processMidi (msg)) return true;

    // ── Faders ────────────────────────────────────────────────────────────────
    for (auto& fader : faders)
        if (fader.processMidi (msg)) return true;

    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
//  fullSync — re-send all LED states to match current DAW state
//
//  Called once after MIDI output is connected and after onLayoutChanged().
//  Walks the visible session ring and sends a Note On for each slot with the
//  correct velocity for its current ClipState.
// ─────────────────────────────────────────────────────────────────────────────
void ApcMiniDriver::fullSync()
{
    // Debounce rapid fullSync requests (e.g. during project load or bulk track additions)
    // to prevent flooding the ALSA MIDI sequencer buffer.
    startTimer (50);
}

void ApcMiniDriver::timerCallback()
{
    stopTimer();
    // Propagate midiOut to sub-components first
    sessionComp.setMidiOutput (midiOut);

    if (midiOut == nullptr)
    {
        fprintf (stderr, "[APC] fullSync — midiOut is nullptr, LED sends skipped\n");
        return;
    }

    fprintf (stderr, "[APC] fullSync — sending LED grid, numTracks=%d\n", app.getNumTracks());

    // u2500u2500 Step 1: unconditionally blank the entire 8u00d78 grid via rawmidi u2500u2500u2500u2500u2500u2500u2500
    // rawmidi sends [0x90, note, 0x00] verbatim; JUCE sequencer would
    // normalise vel=0 to NoteOff (0x80) which the APC Mini ignores.
    for (int note = 0; note < 64; ++note)
        sendLedRaw (rawMidiOut, ApcMini::kChannel, note, 0);

    // ── Step 2: force all lastKnownState to a sentinel value ─────────────────
    // This ensures the dedup guard in onClipStateChanged won't swallow any
    // subsequent state send — including Empty→Empty transitions for new projects.
    sessionComp.invalidateLastKnownState();

    // ── Step 3: re-send every slot via the normal path ───────────────────────
    // onClipStateChanged → sessionComp.onClipStateChanged → sendLed (via stateMapper)
    // Now always fires because every slot differs from the Recording sentinel.
    int numTracks = app.getNumTracks();
    int tOff      = sessionComp.getTrackOffset();
    int sOff      = sessionComp.getSceneOffset();

    for (int lx = 0; lx < 8; ++lx)
    {
        int absTrack = tOff + lx;
        for (int ly = 0; ly < 8; ++ly)
        {
            int absScene = sOff + ly;
            ClipState state = ClipState::Empty;

            if (absTrack < numTracks)
                state = app.getClipState (absTrack, absScene);

            onClipStateChanged (absTrack, absScene, state);
        }
    }

    // Scene buttons: turn off via rawmidi
    for (int s = 0; s < 8; ++s)
        sendLedRaw (rawMidiOut, ApcMini::kChannel, ApcMini::kSceneBase + s, 0);

    // Track buttons: off via rawmidi
    for (int t = 0; t < 8; ++t)
        sendLedRaw (rawMidiOut, ApcMini::kChannel, ApcMini::kTrackBase + t, 0);
}

// ─────────────────────────────────────────────────────────────────────────────
//  ClipStateObserver callbacks
// ─────────────────────────────────────────────────────────────────────────────
void ApcMiniDriver::onClipStateChanged (int trackIdx, int sceneIdx, ClipState state)
{
    fprintf (stderr, "[APC] onClipStateChanged track=%d scene=%d state=%d midiOut=%s\n",
             trackIdx, sceneIdx, (int) state, midiOut != nullptr ? "valid" : "null");

    // Delegate to SessionComponent which handles ring-window translation and
    // redundant-LED suppression via lastKnownState. It will automatically use
    // our stateMapper to send the correct velocity value if the state changed.
    sessionComp.onClipStateChanged (trackIdx, sceneIdx, state);
}

void ApcMiniDriver::onTransportChanged (bool /*isPlaying*/, bool /*isRecording*/)
{
    // APC Mini Mk1 has no dedicated transport LEDs in the standard mapping.
    // Transport state is reflected implicitly through clip state changes.
}

void ApcMiniDriver::onLayoutChanged()
{
    sessionComp.onLayoutChanged();
    // After a layout change, ControlSurfaceManager will issue individual
    // onClipStateChanged calls for every occupied slot.  fullSync() ensures
    // any newly-empty columns are blanked immediately.
    fullSync();
}

// ─────────────────────────────────────────────────────────────────────────────
//  stateToVelocity — APC Mini Mk1 LED colour mapping
// ─────────────────────────────────────────────────────────────────────────────
int ApcMiniDriver::stateToVelocity (ClipState state) const
{
    switch (state)
    {
        case ClipState::Playing:   return ApcMini::kGreen;
        case ClipState::Queued:    return ApcMini::kGreenBlink;
        case ClipState::Recording: return ApcMini::kRed;
        case ClipState::HasClip:   return ApcMini::kYellow;
        case ClipState::Empty:     return ApcMini::kOff;
    }
    return ApcMini::kOff;
}
