#include "ControlSurfaceManager.h"
#include <cstdio>  // fprintf

void ControlSurfaceManager::addSurface (std::unique_ptr<ControlSurface> surface)
{
    jassert (juce::MessageManager::getInstance()->isThisTheMessageThread());

    auto midiOut = findOutputForSurface (*surface);
    if (midiOut != nullptr)
    {
        surface->setMidiOutput (midiOut.get());
        juce::Timer::callAfterDelay (100, [s = surface.get()]() { s->fullSync(); });
    }
    else
    {
        surface->setMidiOutput (nullptr);
        surface->fullSync(); // initialize state even without LEDs
    }

    surfaces.push_back (std::move (surface));
    midiOutputs.push_back (std::move (midiOut)); // may be nullptr if not found

    // Always keep the polling timer running: even if we found an output today,
    // the JUCE UMP layer may mark it dead later (e.g. when AudioDeviceManager
    // opens the same ALSA port as a MIDI input).  refreshOutputs() handles both
    // the null-output and the alive-check cases.
    startTimer (1000);
}

void ControlSurfaceManager::timerCallback()
{
    // Polling mode: only try to open outputs that are currently null.
    // We do NOT force-reopen live outputs here — that would cause unnecessary
    // ALSA churn every second.  Force-reopen is handled by the device-list
    // connection callback (called when JUCE detects a port change).
    refreshOutputs (/*forceReopen=*/false);
}

void ControlSurfaceManager::refreshOutputs (bool forceReopen)
{
    // forceReopen=true  — drop ALL output handles and re-open from scratch.
    //     Used by the MidiDeviceListConnection callback when JUCE signals that
    //     a MIDI device appeared/disappeared, which may have invalidated our
    //     existing connection handle (JUCE's ump::Output has no public isAlive()).
    //
    // forceReopen=false — only open outputs that are currently null (timer path).
    bool needTimer = false;
    for (size_t i = 0; i < surfaces.size(); ++i)
    {
        if (forceReopen || midiOutputs[i] == nullptr)
        {
            // Release first so ALSA doesn't see two simultaneous subscriptions
            // from the same sequencer client to the same port.
            midiOutputs[i].reset();
            surfaces[i]->setMidiOutput (nullptr);

            auto out = findOutputForSurface (*surfaces[i]);
            if (out != nullptr)
            {
                surfaces[i]->setMidiOutput (out.get());
                juce::Timer::callAfterDelay (100, [s = surfaces[i].get()]() { s->fullSync(); });
                midiOutputs[i] = std::move (out);
            }
            else
            {
                needTimer = true;
            }
        }
    }

    if (!needTimer)
        stopTimer();
}

bool ControlSurfaceManager::handleMidi (const juce::MidiMessage& msg)
{
    for (auto& surface : surfaces)
        if (surface->handleMidi (msg))
            return true;
    return false;
}

void ControlSurfaceManager::notifyClipState (int trackIdx, int sceneIdx, ClipState state)
{
    for (auto& surface : surfaces)
        surface->onClipStateChanged (trackIdx, sceneIdx, state);
}

void ControlSurfaceManager::notifyTransport (bool isPlaying, bool isRecording)
{
    for (auto& surface : surfaces)
        surface->onTransportChanged (isPlaying, isRecording);
}

void ControlSurfaceManager::notifyLayout()
{
    for (auto& surface : surfaces)
        surface->onLayoutChanged();
}

std::unique_ptr<juce::MidiOutput> ControlSurfaceManager::findOutputForSurface (const ControlSurface& surface)
{
    const juce::String deviceName = surface.getDeviceName();

    // Diagnostic: dump all visible MIDI output devices
    const auto allOutputs = juce::MidiOutput::getAvailableDevices();
    fprintf (stderr, "[CSM] %d MIDI output(s) visible:\n", allOutputs.size());
    for (const auto& d : allOutputs)
        fprintf (stderr, "  name='%s'  id='%s'\n", d.name.toRawUTF8(), d.identifier.toRawUTF8());

    for (const auto& info : allOutputs)
    {
        if (info.name.containsIgnoreCase (deviceName))
        {
            fprintf (stderr, "[CSM] opening MIDI output '%s' (id='%s') for surface '%s'\n",
                     info.name.toRawUTF8(), info.identifier.toRawUTF8(), deviceName.toRawUTF8());
            auto out = juce::MidiOutput::openDevice (info.identifier);
            if (out == nullptr)
                fprintf (stderr, "[CSM] openDevice FAILED for id='%s'\n", info.identifier.toRawUTF8());
            else
                fprintf (stderr, "[CSM] openDevice OK\n");
            return out;
        }
    }

    fprintf (stderr, "[CSM] no MIDI output matched '%s' — surface will run LED-silent\n", deviceName.toRawUTF8());
    return nullptr;
}
