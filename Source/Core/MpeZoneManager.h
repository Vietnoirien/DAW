#pragma once

#include <JuceHeader.h>
#include <array>
#include "TrackCommand.h"

// ─────────────────────────────────────────────────────────────────────────────
//  MpeZoneManager — message-thread-only MPE zone detector and channel mapper.
//
//  Responsibilities:
//    1. Detect MPE zones from RPN 0 / CC#6 messages OR by auto-detecting
//       simultaneous multi-channel note activity.
//    2. Track which MIDI note is currently active on each member channel.
//    3. Translate incoming PitchBend / ChannelPressure / CC74 messages into
//       a TrackCommand::MpePayload that the caller can push to commandQueue.
//
//  Thread safety: all methods must be called exclusively on the message thread.
//  The render thread never touches this object.
// ─────────────────────────────────────────────────────────────────────────────
class MpeZoneManager
{
public:
    MpeZoneManager()
    {
        channelNote.fill (-1);
        channelPitchBend.fill (0.0f);
        channelPressure.fill  (0.0f);
        channelTimbre.fill    (0.5f);
    }

    // ── Zone configuration ────────────────────────────────────────────────────
    // Call when an RPN 0 + CC#6 MPE Configuration Report is received.
    // numMemberChannels = 1..15 for a lower zone, 0 to clear the zone.
    void configureLowerZone (int numMemberChannels)
    {
        lowerZoneChannels = juce::jlimit (0, 15, numMemberChannels);
        mpeEnabled = (lowerZoneChannels > 0 || upperZoneChannels > 0);
    }

    void configureUpperZone (int numMemberChannels)
    {
        upperZoneChannels = juce::jlimit (0, 15, numMemberChannels);
        mpeEnabled = (lowerZoneChannels > 0 || upperZoneChannels > 0);
    }

    // ── Manual enable/disable toggle (UI) ────────────────────────────────────
    void setEnabled (bool e) { mpeEnabled = e; }
    bool isEnabled()   const { return mpeEnabled; }

    // ── Main parsing entry point ──────────────────────────────────────────────
    // Called from MainComponent::handleIncomingMidiMessage() for every incoming
    // juce::MidiMessage.
    //
    // Returns true  → this was an MPE channel-voice message; outPayload is valid
    //                  and should be pushed to the target track's commandQueue.
    // Returns false → not an MPE expression event (Note On/Off, non-member-ch, …);
    //                  caller should still forward to midiCollector normally.
    //
    // Note On / Note Off on member channels are *also* forwarded to midiCollector
    // (return false) because the instrument needs them for voice allocation.
    // Only CC74 / PB / CP on member channels return true without forwarding.
    bool processMidiMessage (const juce::MidiMessage& msg,
                             TrackCommand::MpePayload& outPayload)
    {
        const int ch = msg.getChannel(); // 1-based

        // ── Auto-detect: if we see two simultaneous Note Ons on different
        //    channels (ch >= 2) while MPE is not yet configured, infer a
        //    14-channel lower zone.  This handles controllers that skip RPN 0.
        if (!mpeEnabled && msg.isNoteOn() && ch >= 2)
        {
            // Check if any other channel already has an active note
            for (int c = 1; c < 16; ++c)
                if (c != ch - 1 && channelNote[c] != -1)
                {
                    configureLowerZone (14); // assume full 14-ch lower zone
                    break;
                }
        }

        // Track Note On/Off so we know which note lives on each channel
        if (msg.isNoteOn())
        {
            channelNote[ch - 1]       = msg.getNoteNumber();
            // Reset per-channel expression on new note
            channelPitchBend[ch - 1]  = 0.0f;
            channelPressure[ch - 1]   = 0.0f;
            channelTimbre[ch - 1]     = 0.5f;
            return false; // Note On still goes to midiCollector
        }
        if (msg.isNoteOff())
        {
            channelNote[ch - 1] = -1;
            return false; // Note Off still goes to midiCollector
        }

        // If MPE is not active, nothing more to do
        if (!mpeEnabled) return false;

        // Only handle member channels (lower zone: ch 2..N+1)
        bool isMemberChannel = (lowerZoneChannels > 0 && ch >= 2 && ch <= lowerZoneChannels + 1)
                            || (upperZoneChannels > 0 && ch <= 15 && ch >= 16 - upperZoneChannels);
        if (!isMemberChannel) return false;

        const int noteId = channelNote[ch - 1];
        if (noteId < 0) return false; // no active note on this channel

        // ── Pitch Bend ───────────────────────────────────────────────────────
        if (msg.isPitchWheel())
        {
            // JUCE: 0..16383, centre = 8192
            float semitones = ((msg.getPitchWheelValue() - 8192) / 8192.0f) * kMaxBendRange;
            channelPitchBend[ch - 1] = semitones;
            outPayload = buildPayload (ch, noteId);
            return true;
        }

        // ── Channel Pressure ─────────────────────────────────────────────────
        if (msg.isChannelPressure())
        {
            channelPressure[ch - 1] = msg.getChannelPressureValue() / 127.0f;
            outPayload = buildPayload (ch, noteId);
            return true;
        }

        // ── CC 74 (Timbre / Slide) ───────────────────────────────────────────
        if (msg.isController() && msg.getControllerNumber() == 74)
        {
            channelTimbre[ch - 1] = msg.getControllerValue() / 127.0f;
            outPayload = buildPayload (ch, noteId);
            return true;
        }

        // RPN 0 / CC#6 — MPE Configuration Report
        // If CC#6 (Data Entry MSB) follows an RPN 0 select, configure zone.
        if (msg.isController())
        {
            const int cc = msg.getControllerNumber();
            if (cc == 100) pendingRpnLsb = msg.getControllerValue();
            if (cc == 101) pendingRpnMsb = msg.getControllerValue();
            if (cc == 6 && pendingRpnMsb == 0 && pendingRpnLsb == 0)
            {
                // MPE Configuration Report: data = number of member channels
                int members = msg.getControllerValue();
                if (ch == 1)       configureLowerZone (members);
                else if (ch == 16) configureUpperZone (members);
            }
        }

        return false;
    }

    // Maximum pitch-bend range in semitones (default ±48).
    // Written by the UI knob, read on the message thread only.
    float kMaxBendRange = 48.0f;

private:
    TrackCommand::MpePayload buildPayload (int ch, int noteId) const
    {
        TrackCommand::MpePayload p;
        p.noteId             = noteId;
        p.channel            = ch;
        p.pitchBendSemitones = channelPitchBend[ch - 1];
        p.pressure           = channelPressure [ch - 1];
        p.timbre             = channelTimbre   [ch - 1];
        return p;
    }

    bool mpeEnabled       = false;
    int  lowerZoneChannels = 0;   // 0 = zone inactive
    int  upperZoneChannels = 0;

    // Per-channel state (index = channel-1, i.e. 0-based)
    std::array<int,   16> channelNote;       // active note (-1 = none)
    std::array<float, 16> channelPitchBend;  // semitones
    std::array<float, 16> channelPressure;   // 0-1
    std::array<float, 16> channelTimbre;     // 0-1

    // Pending RPN tracking for CC#6 parsing
    int pendingRpnMsb = 127;
    int pendingRpnLsb = 127;
};
