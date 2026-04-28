#include "MixerComponent.h"

MixerComponent::MixerComponent (int maxTracks_)
    : maxTracks (maxTracks_)
{
    volumeSliders.assign (maxTracks, nullptr);
    muteButtons.assign   (maxTracks, nullptr);
    soloButtons.assign   (maxTracks, nullptr);
    armButtons.assign    (maxTracks, nullptr);
}

void MixerComponent::setVolumeSlider (int trackIdx, SliderElement* slider)
{
    if (trackIdx < 0 || trackIdx >= maxTracks) return;
    volumeSliders[trackIdx] = slider;
    if (slider == nullptr) return;

    slider->onMoved = [this, trackIdx] (int value)
    {
        // Map 0-127 → 0.0-1.0 gain
        float gain = juce::jlimit (0.0f, 1.0f, value / 127.0f);
        if (onVolumeChanged) onVolumeChanged (trackIdx, gain);
    };
}

void MixerComponent::setMasterSlider (SliderElement* slider)
{
    masterSlider = slider;
    if (slider == nullptr) return;

    slider->onMoved = [this] (int value)
    {
        float gain = juce::jlimit (0.0f, 1.0f, value / 127.0f);
        if (onMasterVolumeChanged) onMasterVolumeChanged (gain);
    };
}

void MixerComponent::setMuteButton (int trackIdx, ButtonElement* btn)
{
    if (trackIdx < 0 || trackIdx >= maxTracks) return;
    muteButtons[trackIdx] = btn;
    if (btn == nullptr) return;

    btn->onPress = [this, trackIdx] (int velocity)
    {
        if (velocity == 0) return;
        // Toggle mute — ControlSurfaceManager reads current state from MainComponent
        // (caller is responsible for toggling and calling back).
        if (onMuteChanged) onMuteChanged (trackIdx, true /* toggle requested */);
    };
}

void MixerComponent::setSoloButton (int trackIdx, ButtonElement* btn)
{
    if (trackIdx < 0 || trackIdx >= maxTracks) return;
    soloButtons[trackIdx] = btn;
    if (btn == nullptr) return;

    btn->onPress = [this, trackIdx] (int velocity)
    {
        if (velocity == 0) return;
        if (onSoloChanged) onSoloChanged (trackIdx, true);
    };
}

void MixerComponent::setArmButton (int trackIdx, ButtonElement* btn)
{
    if (trackIdx < 0 || trackIdx >= maxTracks) return;
    armButtons[trackIdx] = btn;
    if (btn == nullptr) return;

    btn->onPress = [this, trackIdx] (int velocity)
    {
        if (velocity == 0) return;
        if (onArmChanged) onArmChanged (trackIdx, true);
    };
}

void MixerComponent::onClipStateChanged (int /*trackIdx*/, int /*sceneIdx*/, ClipState /*state*/)
{
    // MixerComponent doesn't need to react to individual clip state changes.
    // Arm button LED is updated via onTransportChanged / onLayoutChanged.
}

void MixerComponent::onTransportChanged (bool /*isPlaying*/, bool /*isRecording*/)
{
    // Nothing needed here yet; transport LED handled by the concrete driver.
}

void MixerComponent::onLayoutChanged()
{
    // When tracks are added/removed the driver may need to update arm LED states.
    // Full resync handled at the ControlSurfaceManager level.
}
