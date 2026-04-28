#include "SessionComponent.h"

SessionComponent::SessionComponent (int width, int height)
    : gridWidth (width), gridHeight (height)
{
    // Resize the 2D grids to match hardware dimensions.
    buttons.assign        (width, std::vector<ButtonElement*> (height, nullptr));
    lastKnownState.assign (width, std::vector<ClipState>      (height, ClipState::Empty));
}

void SessionComponent::setSlotButton (int x, int y, ButtonElement* btn)
{
    if (x < 0 || x >= gridWidth || y < 0 || y >= gridHeight)
        return;

    buttons[x][y] = btn;

    if (btn == nullptr) return;

    // Wire the hardware button press → absolute clip launch callback.
    btn->onPress = [this, x, y] (int velocity)
    {
        if (velocity == 0) return; // ignore Note Off / zero CC
        int absTrack = trackOffset + x;
        int absScene = sceneOffset + y;
        if (onSlotPressed) onSlotPressed (absTrack, absScene);
    };
}

void SessionComponent::scrollTrack (int delta)
{
    trackOffset = juce::jmax (0, trackOffset + delta);
    resync();
    if (onRingMoved) onRingMoved();
}

void SessionComponent::scrollScene (int delta)
{
    sceneOffset = juce::jmax (0, sceneOffset + delta);
    resync();
    if (onRingMoved) onRingMoved();
}

void SessionComponent::resync()
{
    // Re-send every visible slot LED based on last known state.
    // Triggered on layout change or device reconnect; not in steady state.
    for (int x = 0; x < gridWidth; ++x)
        for (int y = 0; y < gridHeight; ++y)
            sendLed (x, y, lastKnownState[x][y]);
}

void SessionComponent::invalidateLastKnownState() noexcept
{
    // Set every slot to a sentinel state (Recording is never emitted by the DAW
    // during a blank sync, so the next onClipStateChanged call will always differ
    // and fire sendLed — even for Empty→Empty transitions).
    for (auto& col : lastKnownState)
        std::fill (col.begin(), col.end(), ClipState::Recording);
}

void SessionComponent::onClipStateChanged (int absTrack, int absScene, ClipState state)
{
    int localX, localY;
    if (!toLocal (absTrack, absScene, localX, localY)) return; // outside ring window

    if (lastKnownState[localX][localY] == state) return; // no change → no LED traffic
    lastKnownState[localX][localY] = state;
    sendLed (localX, localY, state);
}

void SessionComponent::onTransportChanged (bool /*isPlaying*/, bool /*isRecording*/)
{
    // Transport state doesn't affect individual clip slot LEDs in this component.
    // Concrete drivers handle transport LED buttons directly.
}

void SessionComponent::onLayoutChanged()
{
    // Grid geometry may have changed (tracks added/removed).
    // Re-fetch everything by resetting last known state and doing a full resync.
    for (auto& col : lastKnownState)
        std::fill (col.begin(), col.end(), ClipState::Empty);

    // ControlSurfaceManager will follow this call with individual
    // onClipStateChanged calls for every occupied slot, rebuilding state.
}

bool SessionComponent::toLocal (int absTrack, int absScene, int& localX, int& localY) const
{
    localX = absTrack - trackOffset;
    localY = absScene - sceneOffset;
    return localX >= 0 && localX < gridWidth
        && localY >= 0 && localY < gridHeight;
}

void SessionComponent::sendLed (int x, int y, ClipState state)
{
    if (buttons[x][y] != nullptr)
    {
        int vel = stateMapper ? stateMapper (state) : defaultLedForState (state);
        buttons[x][y]->sendValue (vel, midiOut);
    }
}

int SessionComponent::defaultLedForState (ClipState state) const
{
    switch (state)
    {
        case ClipState::Playing:   return LedColor::Green;
        case ClipState::Queued:    return LedColor::GreenBlink;
        case ClipState::Recording: return LedColor::Red;
        case ClipState::HasClip:   return LedColor::Amber;
        case ClipState::Empty:     return LedColor::Off;
    }
    return LedColor::Off;
}
