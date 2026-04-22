#pragma once
#include <JuceHeader.h>
#include <functional>
#include "ClipData.h"

// ─── Layout Constants ─────────────────────────────────────────────────────────
static constexpr int SV_HEADER_H  = 36;
static constexpr int SV_SLOT_H    = 36;
static constexpr int SV_MIXER_H   = 220;
static constexpr int SV_SCENE_W   = 52;
static constexpr int SV_RETURN_W  = 80;
static constexpr int SV_MASTER_W  = 80;
static constexpr int SV_TRACK_W   = 180; // fixed track column width

// ─────────────────────────────────────────────────────────────────────────────
// ClipSlot
// ─────────────────────────────────────────────────────────────────────────────
class ClipSlot : public juce::Component
{
public:
    int      trackIndex  = -1;
    int      sceneIndex  = -1;
    ClipData data;
    bool     isSelected  = false;
    bool     isHovered   = false;
    float    playheadPhase = -1.0f;

    std::function<void()> onCreateClip;
    std::function<void()> onSelectClip;
    std::function<void()> onDeleteClip;
    std::function<void(const juce::String&)> onRenameClip;
    std::function<void(juce::Colour)>        onSetClipColour;

    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat().reduced (1.5f);

        if (data.hasClip)
        {
            auto base = data.isPlaying ? data.colour.brighter (0.15f) : data.colour.darker (0.25f);
            g.setColour (base);
            g.fillRoundedRectangle (b, 4.0f);

            if (data.isPlaying)
            {
                g.setColour (juce::Colours::limegreen);
                g.drawRoundedRectangle (b.reduced (1.0f), 4.0f, 2.0f);
            }
            if (isSelected)
            {
                g.setColour (juce::Colours::white);
                g.drawRoundedRectangle (b.reduced (1.5f), 4.0f, 1.5f);
            }

            g.setColour (juce::Colours::white.withAlpha (0.9f));
            g.setFont (juce::Font (juce::FontOptions (11.0f)));
            g.drawText (data.name, b.reduced (24, 0).toNearestInt(), juce::Justification::centredLeft);

            float tx = b.getX() + 12.0f, ty = b.getCentreY();
            juce::Path tri;
            tri.addTriangle (tx - 5.0f, ty - 5.0f, tx - 5.0f, ty + 5.0f, tx + 4.0f, ty);
            g.setColour (data.isPlaying ? juce::Colours::limegreen : juce::Colours::white.withAlpha (0.5f));
            g.fillPath (tri);

            if (data.isPlaying && playheadPhase >= 0.0f) {
                float pieSize = 14.0f;
                float px = b.getRight() - 6.0f - pieSize;
                float py = b.getCentreY() - pieSize * 0.5f;
                
                g.setColour(juce::Colours::black.withAlpha(0.3f));
                g.fillEllipse(px, py, pieSize, pieSize);
                
                g.setColour(juce::Colours::white.withAlpha(0.8f));
                juce::Path pie;
                pie.addPieSegment(px, py, pieSize, pieSize, 0.0f, playheadPhase * juce::MathConstants<float>::twoPi, 0.0f);
                g.fillPath(pie);
            }
        }
        else
        {
            g.setColour (isHovered ? juce::Colour (0xff222238) : juce::Colour (0xff141420));
            g.fillRoundedRectangle (b, 4.0f);
            g.setColour (isHovered ? juce::Colour (0xff5555aa) : juce::Colour (0xff303050));
            g.drawRoundedRectangle (b, 4.0f, 1.0f);

            if (isHovered)
            {
                g.setColour (juce::Colour (0xff7777bb));
                g.setFont (juce::Font (juce::FontOptions (16.0f)));
                g.drawText ("+", b.toNearestInt(), juce::Justification::centred);
            }
        }
    }

    void mouseEnter (const juce::MouseEvent&) override { isHovered = true;  repaint(); }
    void mouseExit  (const juce::MouseEvent&) override { isHovered = false; repaint(); }

    std::function<void()> onLaunchClip;

    // ── Colour palette shared across all clip / track menus ──────────────────
    static void addColourSubmenu (juce::PopupMenu& parent,
                                  std::function<void(juce::Colour)> cb)
    {
        struct Swatch { const char* name; juce::uint32 argb; };
        static constexpr Swatch palette[] = {
            { "Blue",   0xff2d89ef }, { "Purple", 0xff7b68ee },
            { "Teal",   0xff1abc9c }, { "Green",  0xff2ecc71 },
            { "Amber",  0xfff39c12 }, { "Red",    0xffe74c3c },
            { "Pink",   0xffe91e90 }, { "Slate",  0xff607d8b },
        };
        juce::PopupMenu sub;
        for (int i = 0; i < 8; ++i)
        {
            juce::Colour c (palette[i].argb);
            sub.addColouredItem (200 + i, palette[i].name, c);
        }
        parent.addSubMenu ("Set Color", sub);
    }

    static juce::Colour colourFromMenuResult (int result)
    {
        struct Swatch { const char* name; juce::uint32 argb; };
        static constexpr Swatch palette[] = {
            { "Blue",   0xff2d89ef }, { "Purple", 0xff7b68ee },
            { "Teal",   0xff1abc9c }, { "Green",  0xff2ecc71 },
            { "Amber",  0xfff39c12 }, { "Red",    0xffe74c3c },
            { "Pink",   0xffe91e90 }, { "Slate",  0xff607d8b },
        };
        int idx = result - 200;
        if (idx >= 0 && idx < 8) return juce::Colour (palette[idx].argb);
        return juce::Colours::transparentBlack;
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
        {
            if (data.hasClip)
            {
                juce::PopupMenu m;
                m.addItem (1, "Rename Pattern");
                m.addItem (2, "Delete Pattern");
                addColourSubmenu (m, onSetClipColour);
                m.showMenuAsync (juce::PopupMenu::Options(), [this](int result) {
                    if (result == 1)
                    {
                        auto* box = new juce::AlertWindow ("Rename Pattern", "Enter new name:", juce::AlertWindow::NoIcon);
                        box->addTextEditor ("name", data.name);
                        box->addButton ("OK",     1, juce::KeyPress (juce::KeyPress::returnKey));
                        box->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
                        box->enterModalState (true, juce::ModalCallbackFunction::create ([this, box](int r) {
                            if (r == 1)
                            {
                                juce::String n = box->getTextEditorContents ("name").trim();
                                if (n.isNotEmpty() && onRenameClip) onRenameClip (n);
                            }
                            delete box;
                        }), true);
                    }
                    else if (result == 2 && onDeleteClip) onDeleteClip();
                    else if (result >= 200)
                    {
                        juce::Colour c = colourFromMenuResult (result);
                        if (!c.isTransparent() && onSetClipColour) onSetClipColour (c);
                    }
                });
            }
            return;
        }

        if (!data.hasClip) {
            if (onCreateClip) onCreateClip();
        }
        else {
            if (e.x < 24) {
                if (onLaunchClip) onLaunchClip();
            } else {
                if (onSelectClip) onSelectClip();
            }
        }
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Track Header
// ─────────────────────────────────────────────────────────────────────────────
class TrackHeader : public juce::Component
{
public:
    juce::String trackName;
    juce::Colour trackColour   { juce::Colour (0xff2d89ef) };
    TrackType    trackType     = TrackType::Audio;
    bool         hasInstrument = false;
    juce::String instrumentName;
    bool         isSelected    = false;

    std::function<void(const juce::String&)> onRenameTrack;
    std::function<void(juce::Colour)>        onSetTrackColour;

    void paint (juce::Graphics& g) override
    {
        g.fillAll (isSelected ? juce::Colour (0xff2a2a40) : juce::Colour (0xff1e1e30));

        // ── Left-edge colour stripe ───────────────────────────────────────────
        g.setColour (trackColour);
        g.fillRect (0, 0, 4, getHeight());

        g.setColour (juce::Colour (0xff353550));
        g.drawLine (0, (float)getHeight(), (float)getWidth(), (float)getHeight(), 1.0f);

        juce::String badge  = (trackType == TrackType::Audio) ? "A" : "M";
        juce::Colour badgeC = (trackType == TrackType::Audio) ? juce::Colour (0xff3a8aff) : juce::Colour (0xffff8c42);
        auto bBox = juce::Rectangle<int> (4, (getHeight() - 16) / 2, 16, 16);
        g.setColour (badgeC.withAlpha (0.25f));
        g.fillRoundedRectangle (bBox.toFloat(), 3.0f);
        g.setColour (badgeC);
        g.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)));
        g.drawText (badge, bBox, juce::Justification::centred);

        g.setColour (isSelected ? juce::Colours::white : juce::Colours::lightgrey);
        g.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
        g.drawText (trackName, 24, 0, getWidth() - 58, getHeight(), juce::Justification::centredLeft);

        if (hasInstrument)
        {
            auto tag = juce::Rectangle<float> ((float)(getWidth() - 46), (float)(getHeight() / 2) - 7.0f, 42.0f, 14.0f);
            juce::String shortName = "SMPLR";
            juce::Colour tagC = juce::Colour(0xff226644);
            if (instrumentName == "Oscillator") {
                shortName = "OSCL";
                tagC = juce::Colour(0xff1a4a7a);
            } else if (instrumentName == "DrumRack") {
                shortName = "DRUM";
                tagC = juce::Colour(0xff7a4a1a);
            }
            g.setColour (tagC);
            g.fillRoundedRectangle (tag, 3.0f);
            g.setColour (juce::Colours::white);
            g.setFont (juce::Font (juce::FontOptions (8.5f, juce::Font::bold)));
            g.drawText (shortName, tag.toNearestInt(), juce::Justification::centred);
        }
    }

    std::function<void()> onSelectTrack;
    std::function<void()> onDeleteTrack;

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
        {
            juce::PopupMenu m;
            m.addItem (1, "Rename Track");
            m.addItem (2, "Delete Track");
            ClipSlot::addColourSubmenu (m, onSetTrackColour);
            m.showMenuAsync (juce::PopupMenu::Options(), [this](int result) {
                if (result == 1)
                {
                    auto* box = new juce::AlertWindow ("Rename Track", "Enter new name:", juce::AlertWindow::NoIcon);
                    box->addTextEditor ("name", trackName);
                    box->addButton ("OK",     1, juce::KeyPress (juce::KeyPress::returnKey));
                    box->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
                    box->enterModalState (true, juce::ModalCallbackFunction::create ([this, box](int r) {
                        if (r == 1)
                        {
                            juce::String n = box->getTextEditorContents ("name").trim();
                            if (n.isNotEmpty())
                            {
                                trackName = n;
                                repaint();
                                if (onRenameTrack) onRenameTrack (n);
                            }
                        }
                        delete box;
                    }), true);
                }
                else if (result == 2 && onDeleteTrack) onDeleteTrack();
                else if (result >= 200)
                {
                    juce::Colour c = ClipSlot::colourFromMenuResult (result);
                    if (!c.isTransparent())
                    {
                        trackColour = c;
                        repaint();
                        if (onSetTrackColour) onSetTrackColour (c);
                    }
                }
            });
            return;
        }

        if (onSelectTrack) onSelectTrack();
    }

    void mouseDoubleClick (const juce::MouseEvent&) override
    {
        auto* box = new juce::AlertWindow ("Rename Track", "Enter new name:", juce::AlertWindow::NoIcon);
        box->addTextEditor ("name", trackName);
        box->addButton ("OK",     1, juce::KeyPress (juce::KeyPress::returnKey));
        box->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        box->enterModalState (true, juce::ModalCallbackFunction::create ([this, box](int r) {
            if (r == 1)
            {
                juce::String n = box->getTextEditorContents ("name").trim();
                if (n.isNotEmpty())
                {
                    trackName = n;
                    repaint();
                    if (onRenameTrack) onRenameTrack (n);
                }
            }
            delete box;
        }), true);
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Helper — apply consistent look to a M/S toggle button
// ─────────────────────────────────────────────────────────────────────────────
static void styleToggleBtn (juce::TextButton& btn, juce::Colour activeColour)
{
    bool on = btn.getToggleState();
    btn.setColour (juce::TextButton::buttonColourId,
                   on ? activeColour : juce::Colour (0xff1e1e30));
    btn.setColour (juce::TextButton::buttonOnColourId,  activeColour);
    btn.setColour (juce::TextButton::textColourOffId,
                   on ? juce::Colours::black : juce::Colours::grey);
    btn.setColour (juce::TextButton::textColourOnId,    juce::Colours::black);
}

// ─────────────────────────────────────────────────────────────────────────────
// Track Column
// ─────────────────────────────────────────────────────────────────────────────
class TrackColumn : public juce::Component
{
public:
    int        trackIndex = -1;
    bool       isSelected = false;
    TrackHeader header;
    juce::OwnedArray<ClipSlot> slots;
    juce::Slider     volFader;
    juce::Slider     sendAKnob;
    juce::TextButton muteBtn  { "M" };
    juce::TextButton soloBtn  { "S" };
    bool isMuted  = false;
    bool isSoloed = false;
    // Visual data updated from timerCallback on the message thread (no locking needed)
    float rmsLevel  = 0.0f;  // 0..1 VU meter display
    float gainValue = 1.0f;  // mirrors track gain for fader tint

    std::function<void(int scene)> onCreateClipAt;
    std::function<void(int scene)> onSelectClipAt;
    std::function<void(int scene)> onLaunchClipAt;
    std::function<void(int scene)> onDeleteClipAt;
    std::function<void()>          onSelectTrack;
    std::function<void()>          onDeleteTrack;
    std::function<void(float)>     onVolumeChanged;
    std::function<void(float)>     onSendChanged;
    std::function<void(bool)>      onMuteChanged;
    std::function<void(bool)>      onSoloChanged;
    std::function<void(int scene, const juce::String&)> onRenameClipAt;
    std::function<void(int scene, juce::Colour)>        onSetClipColourAt;
    std::function<void(const juce::String&)>            onRenameTrack;
    std::function<void(juce::Colour)>                   onSetTrackColour;

    TrackColumn (int index, const juce::String& name, TrackType type)
        : trackIndex (index)
    {
        header.trackName  = name;
        header.trackType  = type;
        addAndMakeVisible (header);

        for (int s = 0; s < NUM_SCENES; ++s)
        {
            auto* slot        = new ClipSlot();
            slot->trackIndex  = index;
            slot->sceneIndex  = s;
            slot->onCreateClip    = [this, s] { if (onCreateClipAt)    onCreateClipAt (s); };
            slot->onSelectClip    = [this, s] { if (onSelectClipAt)    onSelectClipAt (s); };
            slot->onLaunchClip    = [this, s] { if (onLaunchClipAt)    onLaunchClipAt (s); };
            slot->onDeleteClip    = [this, s] { if (onDeleteClipAt)    onDeleteClipAt (s); };
            slot->onRenameClip    = [this, s] (const juce::String& n) { if (onRenameClipAt)    onRenameClipAt    (s, n); };
            slot->onSetClipColour = [this, s] (juce::Colour c)         { if (onSetClipColourAt) onSetClipColourAt (s, c); };
            addAndMakeVisible (slot);
            slots.add (slot);
        }

        volFader.setSliderStyle (juce::Slider::LinearVertical);
        volFader.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 50, 16);
        volFader.setRange (0.0, 1.0);
        volFader.setValue (1.0);
        volFader.setDoubleClickReturnValue (true, 1.0);
        volFader.onValueChange = [this] {
            if (onVolumeChanged) onVolumeChanged ((float) volFader.getValue());
        };
        addAndMakeVisible (volFader);

        sendAKnob.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        sendAKnob.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        sendAKnob.setDoubleClickReturnValue (true, 0.0);
        sendAKnob.onValueChange = [this] {
            if (onSendChanged) onSendChanged ((float) sendAKnob.getValue());
        };
        addAndMakeVisible (sendAKnob);

        // ── Mute button ───────────────────────────────────────────────────────
        muteBtn.setClickingTogglesState (true);
        styleToggleBtn (muteBtn, juce::Colour (0xffdd8800)); // amber when on
        muteBtn.onClick = [this] {
            isMuted = muteBtn.getToggleState();
            styleToggleBtn (muteBtn, juce::Colour (0xffdd8800));
            if (onMuteChanged) onMuteChanged (isMuted);
        };
        addAndMakeVisible (muteBtn);

        // ── Solo button ───────────────────────────────────────────────────────
        soloBtn.setClickingTogglesState (true);
        styleToggleBtn (soloBtn, juce::Colour (0xff22cc55)); // green when on
        soloBtn.onClick = [this] {
            isSoloed = soloBtn.getToggleState();
            styleToggleBtn (soloBtn, juce::Colour (0xff22cc55));
            if (onSoloChanged) onSoloChanged (isSoloed);
        };
        addAndMakeVisible (soloBtn);
    }

    // ── Public helpers ────────────────────────────────────────────────────────
    void setMuted (bool m)
    {
        isMuted = m;
        muteBtn.setToggleState (m, juce::dontSendNotification);
        styleToggleBtn (muteBtn, juce::Colour (0xffdd8800));
    }

    void setSoloed (bool s)
    {
        isSoloed = s;
        soloBtn.setToggleState (s, juce::dontSendNotification);
        styleToggleBtn (soloBtn, juce::Colour (0xff22cc55));
    }

    void resized() override
    {
        auto b = getLocalBounds();
        header.setBounds (b.removeFromTop (SV_HEADER_H));
        auto mix = b.removeFromBottom (SV_MIXER_H).reduced (4);

        // M/S button row
        auto btnRow = mix.removeFromTop (22);
        int  btnW   = (btnRow.getWidth() - 2) / 2;
        muteBtn.setBounds (btnRow.removeFromLeft (btnW));
        btnRow.removeFromLeft (2);
        soloBtn.setBounds (btnRow);

        mix.removeFromTop (4); // gap between buttons and send label

        // "Send A" label row (so it never overlaps M/S buttons)
        mix.removeFromTop (14); // label area — painted manually in paint()

        // Larger send knob
        sendAKnob.setBounds (mix.removeFromTop (52).withSizeKeepingCentre (44, 44));
        mix.removeFromTop (2); // small gap before fader
        volFader.setBounds (mix);
        for (int s = 0; s < slots.size(); ++s)
            slots[s]->setBounds (b.removeFromTop (SV_SLOT_H).reduced (2));
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (isSelected ? juce::Colour (0xff1a1a2a) : juce::Colour (0xff111120));
        g.setColour (isSelected ? juce::Colour (0xff555577) : juce::Colour (0xff252540));
        g.drawRect (getLocalBounds(), isSelected ? 2 : 1);

        // ── "Send A" label — sits between M/S buttons and the knob ──────────
        g.setColour (juce::Colour (0xff8888aa));
        g.setFont (juce::Font (juce::FontOptions (9.5f, juce::Font::bold)));
        int labelY = getHeight() - SV_MIXER_H + 4 + 22 + 4;
        g.drawText ("SEND A", 0, labelY, getWidth(), 14, juce::Justification::centred);
    }

    // paintOverChildren() is called AFTER child components are rendered,
    // so the gain tint and RMS meter appear on top of the volFader slider.
    void paintOverChildren (juce::Graphics& g) override
    {
        auto fb = volFader.getBounds();

        // The slider component includes a 16px TextBoxBelow — the actual
        // fader track lives in the region above it.
        static constexpr int kTextBoxH = 16;
        const int   trackTop    = fb.getY();
        const int   trackBottom = fb.getBottom() - kTextBoxH;
        const float trackH      = (float)(trackBottom - trackTop);
        if (trackH < 4.0f) return;

        // ── Gain tint: warm amber bar from thumb down to fader track bottom ───
        if (gainValue > 0.01f)
        {
            float gainNorm = juce::jlimit (0.0f, 1.0f, gainValue);
            float thumbY   = trackTop + trackH * (1.0f - gainNorm);
            juce::Rectangle<float> gainBar (
                (float) fb.getCentreX() - 2.0f,
                thumbY,
                4.0f,
                (float) trackBottom - thumbY);
            juce::ColourGradient grad (
                juce::Colour (0xffcc8833).withAlpha (0.45f), gainBar.getX(), gainBar.getY(),
                juce::Colour (0xff995500).withAlpha (0.10f), gainBar.getX(), gainBar.getBottom(),
                false);
            g.setGradientFill (grad);
            g.fillRoundedRectangle (gainBar, 2.0f);
        }

        // ── RMS level meter: bar centered on the fader rail ──────────────────
        if (rmsLevel > 0.002f)
        {
            float lvl    = juce::jlimit (0.0f, 1.0f, rmsLevel);
            float meterH = trackH * lvl;
            float mw     = 5.0f;
            // Centre on the fader rail (same axis as the gain tint)
            float mx     = (float) fb.getCentreX() - mw * 0.5f;
            float my     = (float) trackBottom - meterH;
            juce::Colour meterCol = lvl > 0.85f ? juce::Colour (0xffff4444)
                                  : lvl > 0.65f ? juce::Colour (0xffddcc00)
                                                : juce::Colour (0xff22dd77);
            juce::ColourGradient mGrad (
                meterCol.withAlpha (0.90f), mx, my,
                meterCol.withAlpha (0.30f), mx, (float) trackBottom,
                false);
            g.setGradientFill (mGrad);
            g.fillRoundedRectangle (mx, my, mw, meterH, 2.0f);
        }
    }

    // Repaint only the mixer strip at the bottom — avoids redrawing all clip
    // slots on every timer tick (called from MainComponent::timerCallback).
    void repaintMixerStrip()
    {
        repaint (0, getHeight() - SV_MIXER_H, getWidth(), SV_MIXER_H);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
        {
            juce::PopupMenu m;
            m.addItem (1, "Delete Track");
            m.showMenuAsync (juce::PopupMenu::Options(), [this](int result) {
                if (result == 1 && onDeleteTrack) onDeleteTrack();
            });
            return;
        }

        if (onSelectTrack) onSelectTrack();
    }

    void setClipData (int si, const ClipData& d)
    {
        if (juce::isPositiveAndBelow (si, slots.size()))
        { slots[si]->data = d; slots[si]->repaint(); }
    }

    void setClipSelected (int si)
    {
        for (auto* sl : slots) sl->isSelected = false;
        if (juce::isPositiveAndBelow (si, slots.size())) slots[si]->isSelected = true;
        repaint();
    }

    void clearSelection()
    {
        for (auto* sl : slots) sl->isSelected = false;
        repaint();
    }

    void setPlayhead(float phase)
    {
        for (auto* sl : slots) {
            sl->playheadPhase = phase;
            if (sl->data.isPlaying) sl->repaint();
        }
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Scene Launch Button
// ─────────────────────────────────────────────────────────────────────────────
class SceneLaunchButton : public juce::Component
{
public:
    int  sceneIndex = -1;
    bool isActive   = false;
    bool isHovered  = false;

    std::function<void()> onLaunch;

    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat().reduced (3.0f);
        g.setColour (isActive   ? juce::Colour (0xff2ecc40)
                   : isHovered  ? juce::Colour (0xff2a3a2a)
                                : juce::Colour (0xff1a2a1a));
        g.fillRoundedRectangle (b, 5.0f);
        g.setColour (isActive ? juce::Colour (0xff40ff60) : juce::Colour (0xff303050));
        g.drawRoundedRectangle (b, 5.0f, 1.0f);

        float cx = b.getCentreX(), cy = b.getCentreY(), sz = 6.0f;
        juce::Path tri;
        tri.addTriangle (cx - sz * 0.5f, cy - sz, cx - sz * 0.5f, cy + sz, cx + sz, cy);
        g.setColour (isActive ? juce::Colours::white : juce::Colour (0xff44aa66));
        g.fillPath (tri);
    }

    void mouseEnter (const juce::MouseEvent&) override { isHovered = true;  repaint(); }
    void mouseExit  (const juce::MouseEvent&) override { isHovered = false; repaint(); }
    void mouseDown  (const juce::MouseEvent&) override { if (onLaunch) onLaunch(); }
};

// ─────────────────────────────────────────────────────────────────────────────
// Fixed Track Column (Return / Master)
// ─────────────────────────────────────────────────────────────────────────────
class FixedTrackColumn : public juce::Component
{
public:
    juce::String trackName;
    juce::Slider volFader;
    bool  isSelected = false;
    float rmsLevel   = 0.0f; // updated from timerCallback — message thread only
    std::function<void(float)> onVolumeChanged;

    explicit FixedTrackColumn (const juce::String& name) : trackName (name)
    {
        volFader.setSliderStyle (juce::Slider::LinearVertical);
        volFader.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 50, 16);
        volFader.setRange (0.0, 1.0);
        volFader.setValue (1.0);
        volFader.setDoubleClickReturnValue (true, 1.0);
        volFader.onValueChange = [this] {
            if (onVolumeChanged) onVolumeChanged ((float) volFader.getValue());
        };
        addAndMakeVisible (volFader);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (isSelected ? juce::Colour (0xff22223a) : juce::Colour (0xff161626));
        g.setColour (isSelected ? juce::Colour (0xff555577) : juce::Colour (0xff252540));
        g.drawRect (getLocalBounds(), isSelected ? 2 : 1);
        g.setColour (isSelected ? juce::Colours::white : juce::Colours::lightgrey);
        g.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
        g.drawText (trackName, 0, 0, getWidth(), SV_HEADER_H, juce::Justification::centred);
    }

    // RMS level meter drawn on top of the fader child component
    void paintOverChildren (juce::Graphics& g) override
    {
        if (rmsLevel <= 0.002f) return;

        auto fb = volFader.getBounds();
        static constexpr int kTextBoxH = 16;
        const int   trackTop    = fb.getY();
        const int   trackBottom = fb.getBottom() - kTextBoxH;
        const float trackH      = (float)(trackBottom - trackTop);
        if (trackH < 4.0f) return;

        float lvl    = juce::jlimit (0.0f, 1.0f, rmsLevel);
        float meterH = trackH * lvl;
        float mw     = 5.0f;
        float mx     = (float) fb.getCentreX() - mw * 0.5f;
        float my     = (float) trackBottom - meterH;

        juce::Colour meterCol = lvl > 0.85f ? juce::Colour (0xffff4444)
                              : lvl > 0.65f ? juce::Colour (0xffddcc00)
                                            : juce::Colour (0xff22dd77);
        juce::ColourGradient mGrad (
            meterCol.withAlpha (0.90f), mx, my,
            meterCol.withAlpha (0.30f), mx, (float) trackBottom,
            false);
        g.setGradientFill (mGrad);
        g.fillRoundedRectangle (mx, my, mw, meterH, 2.0f);
    }

    void repaintMixerStrip()
    {
        // Repaint only the fader area — header and scenes don't change on ticks
        int faderTop = SV_HEADER_H + NUM_SCENES * SV_SLOT_H;
        repaint (0, faderTop, getWidth(), getHeight() - faderTop);
    }

    std::function<void()> onSelectTrack;

    void mouseDown (const juce::MouseEvent&) override
    {
        if (onSelectTrack) onSelectTrack();
    }

    void resized() override
    {
        auto b = getLocalBounds();
        b.removeFromTop (SV_HEADER_H + NUM_SCENES * SV_SLOT_H);
        volFader.setBounds (b.reduced (6));
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// TrackGridContent  –  the scrollable inner layer
// ─────────────────────────────────────────────────────────────────────────────
class TrackGridContent : public juce::Component
{
public:
    juce::OwnedArray<TrackColumn> columns;
    bool isDragOver = false; // controlled by SessionView DnD

    std::function<void(int track, int scene)>                          onCreateClip;
    std::function<void(int track, int scene)>                          onSelectClip;
    std::function<void(int track, int scene)>                          onLaunchClip;
    std::function<void(int track, int scene)>                          onDeleteClip;
    std::function<void(int trackIndex)>                                onSelectTrack;
    std::function<void(int trackIndex)>                                onDeleteTrack;
    std::function<void(int trackIndex, const juce::String& type)>      onInstrumentDropped;
    std::function<void(int trackIndex, const juce::String& type)>      onEffectDropped;
    std::function<void(int trackIndex, float gain)>                    onTrackVolumeChanged;
    std::function<void(int trackIndex, float level)>                   onTrackSendChanged;
    std::function<void(int trackIndex, bool muted)>                    onTrackMuteChanged;
    std::function<void(int trackIndex, bool soloed)>                   onTrackSoloChanged;
    std::function<void(int track, int scene, const juce::String&)>     onRenameClip;
    std::function<void(int track, int scene, juce::Colour)>            onSetClipColour;
    std::function<void(int trackIndex, const juce::String&)>           onRenameTrack;
    std::function<void(int trackIndex, juce::Colour)>                  onSetTrackColour;

    // ── Track Management ─────────────────────────────────────────────────────
    int addTrack (TrackType type, const juce::String& name)
    {
        int idx  = columns.size();
        auto* col = new TrackColumn (idx, name, type);
        col->onCreateClipAt = [this, idx] (int s) { if (onCreateClip) onCreateClip (idx, s); };
        col->onSelectClipAt = [this, idx] (int s) { if (onSelectClip) onSelectClip (idx, s); };
        col->onLaunchClipAt = [this, idx] (int s) { if (onLaunchClip) onLaunchClip (idx, s); };
        col->onDeleteClipAt = [this, idx] (int s) { if (onDeleteClip) onDeleteClip (idx, s); };
        col->onSelectTrack  = [this, idx] () { if (onSelectTrack) onSelectTrack (idx); };
        col->onDeleteTrack  = [this, idx] () { if (onDeleteTrack) onDeleteTrack (idx); };
        col->onVolumeChanged = [this, idx] (float g) { if (onTrackVolumeChanged) onTrackVolumeChanged (idx, g); };
        col->onSendChanged   = [this, idx] (float l) { if (onTrackSendChanged)   onTrackSendChanged   (idx, l); };
        col->onMuteChanged      = [this, idx] (bool m)  { if (onTrackMuteChanged)   onTrackMuteChanged   (idx, m); };
        col->onSoloChanged      = [this, idx] (bool s)  { if (onTrackSoloChanged)   onTrackSoloChanged   (idx, s); };
        col->onRenameTrack      = [this, idx] (const juce::String& n) { if (onRenameTrack)    onRenameTrack    (idx, n); };
        col->onSetTrackColour   = [this, idx] (juce::Colour c)         { if (onSetTrackColour) onSetTrackColour (idx, c); };
        col->onRenameClipAt     = [this, idx] (int s, const juce::String& n) { if (onRenameClip)    onRenameClip    (idx, s, n); };
        col->onSetClipColourAt  = [this, idx] (int s, juce::Colour c)         { if (onSetClipColour) onSetClipColour (idx, s, c); };
        col->header.onSelectTrack  = col->onSelectTrack;
        col->header.onDeleteTrack  = col->onDeleteTrack;
        col->header.onRenameTrack  = col->onRenameTrack;
        col->header.onSetTrackColour = col->onSetTrackColour;
        addAndMakeVisible (col);
        columns.add (col);
        // Explicitly re-layout: setSize() in updateContentSize() may not change the
        // total size (if the viewport is wide enough), so resized() would never fire
        // and the new column would get zero bounds. Always force the layout here.
        resized();
        repaint();
        return idx;
    }

    void setClipData (int track, int scene, const ClipData& d)
    {
        if (juce::isPositiveAndBelow (track, columns.size()))
            columns[track]->setClipData (scene, d);
    }

    void setClipSelected (int track, int scene)
    {
        for (auto* c : columns) c->clearSelection();
        if (juce::isPositiveAndBelow (track, columns.size()))
            columns[track]->setClipSelected (scene);
    }

    void setTrackPlayhead(int track, float phase)
    {
        if (juce::isPositiveAndBelow (track, columns.size()))
            columns[track]->setPlayhead(phase);
    }

    void setTrackSelected (int trackIndex)
    {
        for (int i = 0; i < columns.size(); ++i)
        {
            columns[i]->isSelected = (i == trackIndex);
            columns[i]->header.isSelected = (i == trackIndex);
            columns[i]->header.repaint();
            columns[i]->repaint();
        }
    }

    void removeTrack (int index)
    {
        if (juce::isPositiveAndBelow (index, columns.size()))
        {
            columns.remove (index);
            for (int i = index; i < columns.size(); ++i)
            {
                columns[i]->trackIndex = i;
                columns[i]->onCreateClipAt = [this, i] (int s) { if (onCreateClip) onCreateClip (i, s); };
                columns[i]->onSelectClipAt = [this, i] (int s) { if (onSelectClip) onSelectClip (i, s); };
                columns[i]->onDeleteClipAt = [this, i] (int s) { if (onDeleteClip) onDeleteClip (i, s); };
                columns[i]->onSelectTrack  = [this, i] () { if (onSelectTrack) onSelectTrack (i); };
                columns[i]->onDeleteTrack  = [this, i] () { if (onDeleteTrack) onDeleteTrack (i); };
                columns[i]->onRenameTrack    = [this, i] (const juce::String& n) { if (onRenameTrack)    onRenameTrack    (i, n); };
                columns[i]->onSetTrackColour = [this, i] (juce::Colour c)         { if (onSetTrackColour) onSetTrackColour (i, c); };
                columns[i]->onRenameClipAt    = [this, i] (int s, const juce::String& n) { if (onRenameClip)    onRenameClip    (i, s, n); };
                columns[i]->onSetClipColourAt = [this, i] (int s, juce::Colour c)         { if (onSetClipColour) onSetClipColour (i, s, c); };
                columns[i]->header.onSelectTrack  = columns[i]->onSelectTrack;
                columns[i]->header.onDeleteTrack  = columns[i]->onDeleteTrack;
                columns[i]->header.onRenameTrack  = columns[i]->onRenameTrack;
                columns[i]->header.onSetTrackColour = columns[i]->onSetTrackColour;
                for (auto* sl : columns[i]->slots)
                {
                    sl->trackIndex = i;
                    sl->onCreateClip = [this, i, s = sl->sceneIndex] { if (onCreateClip) onCreateClip (i, s); };
                    sl->onSelectClip = [this, i, s = sl->sceneIndex] { if (onSelectClip) onSelectClip (i, s); };
                    sl->onLaunchClip = [this, i, s = sl->sceneIndex] { if (onLaunchClip) onLaunchClip (i, s); };
                    sl->onDeleteClip = [this, i, s = sl->sceneIndex] { if (onDeleteClip) onDeleteClip (i, s); };
                }
            }
            resized();
            repaint();
        }
    }

    // ── Layout ───────────────────────────────────────────────────────────────
    void resized() override
    {
        int h = getHeight();
        for (int i = 0; i < columns.size(); ++i)
            columns[i]->setBounds (i * SV_TRACK_W, 0, SV_TRACK_W, h);
        // drop zone occupies the last SV_TRACK_W px — no component, painted below
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff0e0e1c));

        // Alternating scene row tint
        for (int s = 0; s < NUM_SCENES; ++s)
            if (s % 2 == 1)
            {
                g.setColour (juce::Colour (0x08ffffff));
                g.fillRect (0, SV_HEADER_H + s * SV_SLOT_H, getWidth(), SV_SLOT_H);
            }

        // ── Drop Zone ────────────────────────────────────────────────────────
        int dzX = columns.size() * SV_TRACK_W;
        int dzH = getHeight();

        g.setColour (isDragOver ? juce::Colour (0x261a7a4a) : juce::Colour (0xff0d0d1a));
        g.fillRect (dzX, 0, SV_TRACK_W, dzH);

        // Dashed border
        juce::Path solid;
        solid.addRectangle ((float)dzX + 5.0f, 5.0f, (float)SV_TRACK_W - 10.0f, (float)dzH - 10.0f);
        juce::Path dashed;
        float dash[] = { 5.0f, 4.0f };
        juce::PathStrokeType (1.3f).createDashedStroke (dashed, solid, dash, 2);
        g.setColour (isDragOver ? juce::Colour (0xff44aa77) : juce::Colour (0xff2a2a4a));
        g.strokePath (dashed, juce::PathStrokeType (1.3f));

        // + icon
        g.setFont (juce::Font (juce::FontOptions (22.0f)));
        g.setColour (isDragOver ? juce::Colour (0xff55dd99) : juce::Colour (0xff2e2e5a));
        g.drawText ("+", dzX, dzH / 2 - 20, SV_TRACK_W, 28, juce::Justification::centred);

        g.setFont (juce::Font (juce::FontOptions (10.0f)));
        g.setColour (juce::Colour (0xff2e2e5a));
        g.drawText ("Drop instrument", dzX, dzH / 2 + 10, SV_TRACK_W, 20, juce::Justification::centred);
    }

private:
    void setDragOver (juce::Point<int> pos)
    {
        bool over = pos.x >= columns.size() * SV_TRACK_W;
        if (over != isDragOver) { isDragOver = over; repaint(); }
    }
};


// ─────────────────────────────────────────────────────────────────────────────
// Session View
// ─────────────────────────────────────────────────────────────────────────────
class SessionView : public juce::Component,
                    public juce::DragAndDropTarget
{
public:
    TrackGridContent gridContent; // public so MainComponent can access columns

    std::function<void(int track, int scene)> onCreateClip;
    std::function<void(int track, int scene)> onSelectClip;
    std::function<void(int track, int scene)> onLaunchClip;
    std::function<void(int track, int scene)> onDeleteClip;
    std::function<void(int track)>             onSelectTrack;
    std::function<void(int track)>             onDeleteTrack;
    std::function<void(int sceneIndex)>        onLaunchScene;
    std::function<void(int trackIndex, const juce::String& instrumentType)> onInstrumentDropped;
    std::function<void(int trackIndex, const juce::String& effectType)>     onEffectDropped;
    std::function<void(int trackIndex, float gain)> onTrackVolumeChanged;
    std::function<void(int trackIndex, float level)> onTrackSendChanged;
    std::function<void(float gain)>                 onMasterVolumeChanged;
    std::function<void(float gain)>                 onReturnVolumeChanged;
    std::function<void(int trackIndex, bool muted)>  onTrackMuteChanged;
    std::function<void(int trackIndex, bool soloed)> onTrackSoloChanged;
    std::function<void(int track, int scene, const juce::String&)> onRenameClip;
    std::function<void(int track, int scene, juce::Colour)>        onSetClipColour;
    std::function<void(int trackIndex, const juce::String&)>       onRenameTrack;
    std::function<void(int trackIndex, juce::Colour)>              onSetTrackColour;

    SessionView()
    {
        // Wire grid content → session callbacks (forward pattern)
        gridContent.onCreateClip = [this] (int t, int s) { if (onCreateClip)         onCreateClip (t, s); };
        gridContent.onSelectClip = [this] (int t, int s) { if (onSelectClip)         onSelectClip (t, s); };
        gridContent.onLaunchClip = [this] (int t, int s) { if (onLaunchClip)         onLaunchClip (t, s); };
        gridContent.onDeleteClip = [this] (int t, int s) { if (onDeleteClip)         onDeleteClip (t, s); };
        gridContent.onSelectTrack = [this] (int t)       { if (onSelectTrack)        onSelectTrack(t); };
        gridContent.onDeleteTrack = [this] (int t)       { if (onDeleteTrack)        onDeleteTrack(t); };
        gridContent.onInstrumentDropped = [this] (int t, const juce::String& tp)
        {
            if (onInstrumentDropped) onInstrumentDropped (t, tp);
        };
        gridContent.onEffectDropped = [this] (int t, const juce::String& tp)
        {
            if (onEffectDropped) onEffectDropped (t, tp);
        };
        gridContent.onTrackVolumeChanged = [this] (int t, float g)
        {
            if (onTrackVolumeChanged) onTrackVolumeChanged (t, g);
        };
        gridContent.onTrackSendChanged = [this] (int t, float l)
        {
            if (onTrackSendChanged) onTrackSendChanged (t, l);
        };
        gridContent.onTrackMuteChanged = [this] (int t, bool m)
        {
            if (onTrackMuteChanged) onTrackMuteChanged (t, m);
        };
        gridContent.onTrackSoloChanged = [this] (int t, bool s)
        {
            if (onTrackSoloChanged) onTrackSoloChanged (t, s);
        };
        gridContent.onRenameClip = [this] (int t, int s, const juce::String& n)
        {
            if (onRenameClip) onRenameClip (t, s, n);
        };
        gridContent.onSetClipColour = [this] (int t, int s, juce::Colour c)
        {
            if (onSetClipColour) onSetClipColour (t, s, c);
        };
        gridContent.onRenameTrack = [this] (int t, const juce::String& n)
        {
            if (onRenameTrack) onRenameTrack (t, n);
        };
        gridContent.onSetTrackColour = [this] (int t, juce::Colour c)
        {
            if (onSetTrackColour) onSetTrackColour (t, c);
        };

        // Viewport – horizontal scroll only
        gridViewport.setViewedComponent (&gridContent, false);
        gridViewport.setScrollBarsShown (false, true);
        gridViewport.getHorizontalScrollBar().setColour (
            juce::ScrollBar::thumbColourId, juce::Colour (0xff334466));
        addAndMakeVisible (gridViewport);

        // Scene buttons
        for (int s = 0; s < NUM_SCENES; ++s)
        {
            auto* btn      = new SceneLaunchButton();
            btn->sceneIndex = s;
            btn->onLaunch  = [this, s] { if (onLaunchScene) onLaunchScene (s); };
            addAndMakeVisible (btn);
            sceneButtons.add (btn);
        }

        returnColumn = std::make_unique<FixedTrackColumn> ("Return A");
        masterColumn = std::make_unique<FixedTrackColumn> ("Master");
        returnColumn->onVolumeChanged = [this] (float g) { if (onReturnVolumeChanged) onReturnVolumeChanged (g); };
        returnColumn->onSelectTrack = [this] () { if (onSelectTrack) onSelectTrack(999); }; // Use 999 for return track index convention
        masterColumn->onVolumeChanged = [this] (float g) { if (onMasterVolumeChanged) onMasterVolumeChanged (g); };
        addAndMakeVisible (returnColumn.get());
        addAndMakeVisible (masterColumn.get());
    }

    // ── Public API ────────────────────────────────────────────────────────────
    int addTrack (TrackType type, const juce::String& name)
    {
        int idx = gridContent.addTrack (type, name);
        updateContentSize();
        return idx;
    }

    void setClipData    (int t, int s, const ClipData& d) { gridContent.setClipData    (t, s, d); }
    void setClipSelected(int t, int s)                    { gridContent.setClipSelected (t, s); }
    void setTrackSelected(int t)                          
    { 
        gridContent.setTrackSelected(t); 
        if (t == 999) {
            returnColumn->isSelected = true;
            returnColumn->repaint();
        } else {
            returnColumn->isSelected = false;
            returnColumn->repaint();
        }
    }
    void setTrackPlayhead(int t, float phase)             { gridContent.setTrackPlayhead(t, phase); }

    void removeTrack(int index) {
        gridContent.removeTrack(index);
        updateContentSize();
    }

    void setSceneActive (int scene, bool active)
    {
        for (auto* b : sceneButtons) { b->isActive = false; b->repaint(); }
        if (active && juce::isPositiveAndBelow (scene, sceneButtons.size()))
            { sceneButtons[scene]->isActive = true; sceneButtons[scene]->repaint(); }
    }

    void setReturnRmsLevel (float lvl)
    {
        returnColumn->rmsLevel = lvl;
        returnColumn->repaintMixerStrip();
    }

    void setMasterRmsLevel (float lvl)
    {
        masterColumn->rmsLevel = lvl;
        masterColumn->repaintMixerStrip();
    }

    // ── Layout ────────────────────────────────────────────────────────────────
    void resized() override
    {
        auto b = getLocalBounds();

        auto sceneStrip = b.removeFromRight (SV_SCENE_W);
        auto masterB    = b.removeFromRight (SV_MASTER_W);
        auto returnB    = b.removeFromRight (SV_RETURN_W);

        masterColumn->setBounds (masterB);
        returnColumn->setBounds (returnB);

        for (int s = 0; s < NUM_SCENES; ++s)
            sceneButtons[s]->setBounds (sceneStrip.getX(), SV_HEADER_H + s * SV_SLOT_H, SV_SCENE_W, SV_SLOT_H);

        gridViewport.setBounds (b);
        updateContentSize();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff0e0e1c));

        // SCENE label above the launch buttons
        g.setColour (juce::Colour (0xff445544));
        g.setFont (juce::Font (juce::FontOptions (9.0f, juce::Font::bold)));
        g.drawText ("SCENE", getWidth() - SV_SCENE_W, 0, SV_SCENE_W, SV_HEADER_H, juce::Justification::centred);

        // Header separator
        g.setColour (juce::Colour (0xff252540));
        g.drawHorizontalLine (SV_HEADER_H, 0.0f, (float)getWidth());
    }

    // ── DragAndDropTarget (on SessionView so Viewport doesn't block delivery) ─
    bool isInterestedInDragSource (const SourceDetails& d) override
    {
        return d.description.toString().startsWith ("InstrumentDrag:") || 
               d.description.toString().startsWith ("EffectDrag:");
    }

    void itemDragEnter (const SourceDetails& d) override { updateContentDrag (d.localPosition, true); }
    void itemDragMove  (const SourceDetails& d) override { updateContentDrag (d.localPosition, true); }
    void itemDragExit  (const SourceDetails&)   override { gridContent.isDragOver = false; gridContent.repaint(); }

    void itemDropped (const SourceDetails& d) override
    {
        gridContent.isDragOver = false;
        gridContent.repaint();

        auto type    = d.description.toString().fromFirstOccurrenceOf (":", false, false);
        auto contPos = toContentCoords (d.localPosition);

        int hit = -1;
        for (int i = 0; i < gridContent.columns.size(); ++i)
            if (gridContent.columns[i]->getBounds().contains (contPos)) { hit = i; break; }

        if (d.description.toString().startsWith ("InstrumentDrag:")) {
            if (onInstrumentDropped) onInstrumentDropped (hit, type);
        } else if (d.description.toString().startsWith ("EffectDrag:")) {
            if (hit == -1) {
                // Check if it's over Return track
                if (returnColumn->getBounds().contains(d.localPosition)) {
                    hit = 999;
                }
            }
            if (onEffectDropped) onEffectDropped (hit, type);
        }
    }

private:
    juce::Viewport                      gridViewport;
    juce::OwnedArray<SceneLaunchButton> sceneButtons;
    std::unique_ptr<FixedTrackColumn>   returnColumn;
    std::unique_ptr<FixedTrackColumn>   masterColumn;

    void updateContentSize()
    {
        auto vb = gridViewport.getBounds();
        if (vb.isEmpty()) return;

        int numCols   = gridContent.columns.size() + 1; // +1 always-visible drop zone
        int contentW  = juce::jmax (vb.getWidth(), numCols * SV_TRACK_W);
        bool needsBar = contentW > vb.getWidth();
        int  barH     = needsBar ? gridViewport.getScrollBarThickness() : 0;

        gridContent.setSize (contentW, vb.getHeight() - barH);
    }

    // Convert a SessionView-local point into TrackGridContent coordinates
    juce::Point<int> toContentCoords (juce::Point<int> svLocal) const
    {
        auto vpOrigin = gridViewport.getBounds().getTopLeft();
        auto scroll   = gridViewport.getViewPosition();
        return svLocal - vpOrigin + scroll;
    }

    // Update the drop-zone highlight on gridContent
    void updateContentDrag (juce::Point<int> svLocal, bool /*entering*/)
    {
        auto pos  = toContentCoords (svLocal);
        bool over = pos.x >= gridContent.columns.size() * SV_TRACK_W
                    && gridViewport.getBounds().contains (svLocal);
        if (over != gridContent.isDragOver)
        {
            gridContent.isDragOver = over;
            gridContent.repaint();
        }
    }
};
