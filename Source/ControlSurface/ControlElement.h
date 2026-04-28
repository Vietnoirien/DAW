#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  ControlElement.h
//
//  Physical abstraction layer: one ButtonElement per hardware button,
//  one SliderElement per hardware fader / encoder.
//
//  Each element:
//    • Matches incoming MIDI messages by channel + note/CC.
//    • Calls an assigned callback lambda when matched.
//    • Can send LED / value feedback to the hardware via sendValue().
//
//  ControlElement instances are owned by concrete ControlSurface drivers
//  (e.g. ApcMiniDriver) and are referenced (not owned) by logical Components
//  (SessionComponent, MixerComponent).
// ─────────────────────────────────────────────────────────────────────────────

#include <JuceHeader.h>

// ── Base ──────────────────────────────────────────────────────────────────────
class ControlElement
{
public:
    virtual ~ControlElement() = default;

    // Returns true if this element consumed the message.
    virtual bool processMidi (const juce::MidiMessage& msg) = 0;
};

// ── Button ────────────────────────────────────────────────────────────────────
class ButtonElement : public ControlElement
{
public:
    // isCC == false → matched by Note On/Off; true → matched by CC number.
    ButtonElement (int midiChannel, int noteOrCC, bool isCC = false)
        : channel (midiChannel), id (noteOrCC), usesCC (isCC) {}

    // Called by the surface driver when a button is pressed.
    // velocity 0 == released (Note Off), >0 == pressed.
    std::function<void (int velocity)> onPress;

    // Send a value (LED color / on-off) back to the hardware.
    // Does nothing if midiOut is nullptr (surface has no MIDI output).
    void sendValue (int value, juce::MidiOutput* midiOut)
    {
        if (midiOut == nullptr) return;

        juce::MidiMessage msg;
        if (usesCC)
            msg = juce::MidiMessage::controllerEvent (channel, id, juce::jlimit (0, 127, value));
        else
            msg = juce::MidiMessage::noteOn (channel, id, (juce::uint8) juce::jlimit (0, 127, value));

        midiOut->sendMessageNow (msg);

    }

    bool processMidi (const juce::MidiMessage& msg) override
    {
        if (usesCC)
        {
            if (msg.isController() && msg.getChannel() == channel && msg.getControllerNumber() == id)
            {
                if (onPress)
                {
                    int vel = msg.getControllerValue();
                    auto cb = onPress;
                    juce::MessageManager::callAsync ([cb, vel]() { cb (vel); });
                }
                return true;
            }
        }
        else
        {
            if ((msg.isNoteOn() || msg.isNoteOff()) && msg.getChannel() == channel && msg.getNoteNumber() == id)
            {
                if (onPress)
                {
                    int vel = msg.isNoteOn() ? msg.getVelocity() : 0;
                    auto cb = onPress;
                    juce::MessageManager::callAsync ([cb, vel]() { cb (vel); });
                }
                return true;
            }
        }
        return false;
    }

    int getChannel()  const noexcept { return channel; }
    int getId()       const noexcept { return id; }
    bool isCCBased()  const noexcept { return usesCC; }

private:
    int  channel;
    int  id;
    bool usesCC;
};

// ── Slider / Encoder ──────────────────────────────────────────────────────────
class SliderElement : public ControlElement
{
public:
    SliderElement (int midiChannel, int ccNumber)
        : channel (midiChannel), cc (ccNumber) {}

    // Called whenever the fader / knob value changes (0-127).
    std::function<void (int value)> onMoved;

    // Send a value to the hardware (motorised faders, encoder rings, etc.).
    void sendValue (int value, juce::MidiOutput* midiOut)
    {
        if (midiOut == nullptr) return;
        midiOut->sendMessageNow (juce::MidiMessage::controllerEvent (channel, cc, juce::jlimit (0, 127, value)));
    }

    bool processMidi (const juce::MidiMessage& msg) override
    {
        if (msg.isController() && msg.getChannel() == channel && msg.getControllerNumber() == cc)
        {
            if (onMoved)
            {
                int val = msg.getControllerValue();
                auto cb = onMoved;
                juce::MessageManager::callAsync ([cb, val]() { cb (val); });
            }
            return true;
        }
        return false;
    }

    int getChannel() const noexcept { return channel; }
    int getCC()      const noexcept { return cc; }

private:
    int channel;
    int cc;
};
