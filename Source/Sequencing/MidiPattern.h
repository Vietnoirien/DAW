#pragma once

#include <JuceHeader.h>
#include "Pattern.h"
#include "ClipData.h"
#include <vector>
#include <algorithm>

class MidiPattern : public Pattern {
public:
    MidiPattern() {
        notes.reserve(256);
    }

    // ── Set the full note list (replaces previous content) ──────────────────
    void setNotes(const std::vector<MidiNote>& newNotes, double lengthBars = 1.0) {
        notes = newNotes;
        patternLengthBars = lengthBars;
    }

    // ── Legacy single-event API (kept for compatibility) ────────────────────
    void addEvent(const juce::MidiMessage& msg, double startBeat) {
        if (msg.isNoteOn())
            notes.push_back({ msg.getNoteNumber(), startBeat, 0.125, msg.getFloatVelocity() });
    }

    void clear() override { notes.clear(); }

    double getLengthBeats() const override {
        return patternLengthBars * 4.0;
    }

    // ── Real-time render ─────────────────────────────────────────────────────
    void getEventsForBuffer(juce::MidiBuffer& output,
                            int64_t blockStartSample, int numSamples,
                            const GlobalTransport& transport,
                            double patternStartSample) const override
    {
        double spb = transport.getSamplesPerBeat();
        if (spb <= 0.0) return;

        double patternLengthSamples = getLengthBeats() * spb;
        if (patternLengthSamples <= 0.0) return;

        if (static_cast<double>(blockStartSample + numSamples) <= patternStartSample) return;

        double blockStart = static_cast<double>(blockStartSample) - patternStartSample;
        double blockEnd   = blockStart + numSamples;
        if (blockStart < 0.0) blockStart = 0.0;

        for (const auto& n : notes) {
            double noteOnSamples  = n.startBeat * spb;
            double noteOffSamples = (n.startBeat + n.lengthBeats) * spb;

            auto scheduleEvent = [&](double offsetInPattern, bool isOn) {
                if (offsetInPattern < 0.0) return;

                // NoteOn:  must be strictly inside the pattern [0, L)
                // NoteOff: allowed at the boundary [0, L] so a note filling
                //          the whole bar still gets its release event.
                if (isOn  && offsetInPattern >= patternLengthSamples) return;
                if (!isOn && offsetInPattern >  patternLengthSamples) return;

                long kS = static_cast<long>(std::ceil((blockStart - offsetInPattern) / patternLengthSamples)) - 1;
                long kE = static_cast<long>(std::ceil((blockEnd   - offsetInPattern) / patternLengthSamples)) + 1;

                // For a NoteOff sitting exactly on the pattern boundary
                // (offsetInPattern == patternLengthSamples), k=-1 would produce
                // exact=0 and alias the event to the pattern's very first sample.
                // Clamp to k>=0 to prevent that.
                if (!isOn && offsetInPattern >= patternLengthSamples)
                    kS = std::max(kS, 0L);

                for (long k = kS; k <= kE; ++k) {
                    double exact = k * patternLengthSamples + offsetInPattern;
                    int64_t gs  = static_cast<int64_t>(std::round(exact + patternStartSample));
                    if (gs >= blockStartSample && gs < blockStartSample + numSamples) {
                        int off = static_cast<int>(gs - blockStartSample);
                        if (isOn)
                            output.addEvent(juce::MidiMessage::noteOn (1, n.note, n.velocity), off);
                        else
                            output.addEvent(juce::MidiMessage::noteOff(1, n.note, 0.0f),        off);
                    }
                }
            };

            scheduleEvent(noteOnSamples,  true);
            scheduleEvent(noteOffSamples, false);
        }
    }

private:
    std::vector<MidiNote> notes;
    double patternLengthBars { 1.0 };
};
