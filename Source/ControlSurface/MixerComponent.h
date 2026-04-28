#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  MixerComponent.h
//
//  Binds hardware sliders and buttons to track mixer parameters in
//  MainComponent's audio engine.
//
//  Writable parameters:
//    • Volume (gain)   — directly writes Track::gain atomic
//    • Mute            — writes Track::muted atomic
//    • Solo            — writes Track::soloed atomic
//    • Arm             — writes Track::isArmedForRecord atomic
//
//  The component does NOT own elements.  Concrete drivers assign elements.
//  Callbacks (onVolumeChanged, etc.) are set by ControlSurfaceManager and
//  forward to MainComponent lambdas already wired in SessionView.
// ─────────────────────────────────────────────────────────────────────────────

#include <JuceHeader.h>
#include "ControlElement.h"
#include "ClipStateObserver.h"

class MixerComponent : public ClipStateObserver
{
public:
    // maxTracks: number of fader slots on the hardware (e.g. 8 for APC Mini)
    explicit MixerComponent (int maxTracks);

    // ── Element binding ───────────────────────────────────────────────────────
    // All track indices are 0-based absolute indices into audioTracks[].
    // The "bank offset" concept can be added in a later phase.

    void setVolumeSlider  (int trackIdx, SliderElement* slider);
    void setMasterSlider  (SliderElement* masterSlider);
    void setMuteButton    (int trackIdx, ButtonElement* btn);
    void setSoloButton    (int trackIdx, ButtonElement* btn);
    void setArmButton     (int trackIdx, ButtonElement* btn);

    // ── DAW callbacks (set by ControlSurfaceManager) ──────────────────────────
    // These mirror the onTrackVolumeChanged etc. already wired in MainComponent.
    std::function<void (int trackIdx, float gain)>  onVolumeChanged;
    std::function<void (float gain)>                onMasterVolumeChanged;
    std::function<void (int trackIdx, bool muted)>  onMuteChanged;
    std::function<void (int trackIdx, bool soloed)> onSoloChanged;
    std::function<void (int trackIdx, bool armed)>  onArmChanged;

    // ── ClipStateObserver ─────────────────────────────────────────────────────
    void onClipStateChanged (int trackIdx, int sceneIdx, ClipState state) override;
    void onTransportChanged (bool isPlaying, bool isRecording) override;
    void onLayoutChanged() override;

private:
    int maxTracks;

    std::vector<SliderElement*> volumeSliders; // indexed by trackIdx
    SliderElement*              masterSlider = nullptr;
    std::vector<ButtonElement*> muteButtons;
    std::vector<ButtonElement*> soloButtons;
    std::vector<ButtonElement*> armButtons;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MixerComponent)
};
