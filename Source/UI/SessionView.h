#pragma once
#include <JuceHeader.h>
#include <functional>
#include "ClipData.h"

// ─── Layout Constants ─────────────────────────────────────────────────────────
static constexpr int SV_HEADER_H        = 36;
static constexpr int SV_SLOT_H          = 36;
static constexpr int SV_ADD_SCENE_BTN_H = 28;  // height of the per-column '+ Scene' button
static constexpr int SV_MIXER_H         = 220;
static constexpr int SV_SCENE_W         = 52;
static constexpr int SV_RETURN_W        = 80;
static constexpr int SV_MASTER_W        = 80;
static constexpr int SV_TRACK_W         = 180; // fixed track column width

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
    std::function<void()> onDuplicateClip;
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
            if (data.isPlaying)
            {
                // Pause icon: two vertical bars
                g.setColour (juce::Colours::limegreen);
                float bw = 3.0f, bh = 10.0f, gap = 3.0f;
                float lx = tx - gap * 0.5f - bw;
                float rx = tx + gap * 0.5f;
                g.fillRect (lx, ty - bh * 0.5f, bw, bh);
                g.fillRect (rx, ty - bh * 0.5f, bw, bh);
            }
            else
            {
                // Play triangle
                juce::Path tri;
                tri.addTriangle (tx - 5.0f, ty - 5.0f, tx - 5.0f, ty + 5.0f, tx + 4.0f, ty);
                g.setColour (juce::Colours::white.withAlpha (0.5f));
                g.fillPath (tri);
            }

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
    std::function<void()> onPauseClip;

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
                m.addItem (3, "Duplicate Pattern");
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
                    else if (result == 3 && onDuplicateClip) onDuplicateClip();
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
                if (data.isPlaying) {
                    if (onPauseClip) onPauseClip();
                } else {
                    if (onLaunchClip) onLaunchClip();
                }
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

    // ── Group membership badge ────────────────────────────────────────────────
    juce::String groupBadgeText;          // e.g. "G1", empty = no group
    juce::Colour groupBadgeColour { juce::Colour(0xff44aa88) };

    std::function<void(const juce::String&)> onRenameTrack;
    std::function<void(juce::Colour)>        onSetTrackColour;
    // Returns [ "No Group", "Group 1", …, "New Group…" ] for the submenu
    std::function<juce::StringArray()>       getGroupNames;
    // groupIdx: -1 = remove from group, 0..N-1 = existing group, N = create new
    std::function<void(int groupIdx)>        onAssignToGroup;

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
            juce::String shortName;
            juce::Colour tagC;
            if (instrumentName == "Oscillator") {
                shortName = "OSCL";
                tagC = juce::Colour(0xff1a4a7a);   // blue
            } else if (instrumentName == "DrumRack") {
                shortName = "DRUM";
                tagC = juce::Colour(0xff7a4a1a);   // orange-brown
            } else if (instrumentName == "Simpler") {
                shortName = "SMPLR";
                tagC = juce::Colour(0xff226644);   // green
            } else if (instrumentName == "FMSynth") {
                shortName = "FM";
                tagC = juce::Colour(0xff1a6a6a);   // teal
            } else if (instrumentName == "WavetableSynth") {
                shortName = "WVTB";
                tagC = juce::Colour(0xff3a2a7a);   // indigo
            } else if (instrumentName == "KarplusStrong") {
                shortName = "KPLS";
                tagC = juce::Colour(0xff7a2a4a);   // rose
            } else if (instrumentName.startsWith ("Plugin:")) {
                // External VST/AU plugin — show first 4 chars of plugin name
                shortName = instrumentName.substring (7).trim().substring (0, 4).toUpperCase();
                tagC = juce::Colour(0xff5a2a7a);   // purple
            } else {
                // Unknown / generic instrument
                shortName = "INST";
                tagC = juce::Colour(0xff3a3a7a);   // dark blue
            }
            g.setColour (tagC);
            g.fillRoundedRectangle (tag, 3.0f);
            g.setColour (juce::Colours::white);
            g.setFont (juce::Font (juce::FontOptions (8.5f, juce::Font::bold)));
            g.drawText (shortName, tag.toNearestInt(), juce::Justification::centred);
        }

        // ── Group membership badge (top-right pill) ───────────────────────────
        if (groupBadgeText.isNotEmpty())
        {
            auto pill = juce::Rectangle<float> ((float)(getWidth() - 26), 3.0f, 22.0f, 12.0f);
            g.setColour (groupBadgeColour.withAlpha (0.85f));
            g.fillRoundedRectangle (pill, 3.0f);
            g.setColour (juce::Colours::white);
            g.setFont   (juce::Font (juce::FontOptions (8.0f, juce::Font::bold)));
            g.drawText  (groupBadgeText, pill.toNearestInt(), juce::Justification::centred);
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

            // ── Assign to Group submenu ───────────────────────────────────────
            if (getGroupNames)
            {
                juce::PopupMenu groupSub;
                auto names = getGroupNames();
                // item 300 = "No Group", 301..N = existing groups, last = "New Group…"
                groupSub.addItem (300, "No Group",   true, groupBadgeText.isEmpty());
                groupSub.addSeparator();
                for (int gi = 1; gi < names.size() - 1; ++gi)    // skip index 0 ("No Group") and last ("New Group…")
                    groupSub.addItem (300 + gi, names[gi], true, groupBadgeText == "G" + juce::String(gi));
                groupSub.addSeparator();
                groupSub.addItem (300 + names.size() - 1, "New Group\u2026");
                m.addSeparator();
                m.addSubMenu ("Assign to Group", groupSub);
            }

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
                else if (result >= 300 && onAssignToGroup)
                {
                    // 300 = "No Group" → -1, 301..N = group index 0..N-1, 300+N = "New Group"
                    onAssignToGroup (result - 301); // -1 means "No Group"
                }
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
    struct SlotsContent : public juce::Component
    {
        void paint (juce::Graphics& g) override { g.fillAll (juce::Colour (0xff111120)); }
    };

public:
    int        trackIndex = -1;
    bool       isSelected = false;
    TrackHeader header;
    juce::OwnedArray<ClipSlot> slots;
    juce::Slider     volFader;
    juce::OwnedArray<juce::Slider> sendKnobs;
    juce::TextButton muteBtn     { "M" };
    juce::TextButton soloBtn     { "S" };
    juce::TextButton recordArmBtn{ "\u25cf" };
    juce::TextButton addSceneBtn { "+" };
    bool isMuted  = false;
    bool isSoloed = false;
    bool isArmed  = false;
    float rmsLevel  = 0.0f;
    float gainValue = 1.0f;

    juce::Viewport slotsViewport;
    SlotsContent   slotsContent;

    std::function<void(int scene)> onCreateClipAt;
    std::function<void(int scene)> onSelectClipAt;
    std::function<void(int scene)> onLaunchClipAt;
    std::function<void(int scene)> onPauseClipAt;
    std::function<void(int scene)> onDeleteClipAt;
    std::function<void(int scene)> onDuplicateClipAt;
    std::function<void()>          onSelectTrack;
    std::function<void()>          onDeleteTrack;
    std::function<void(float)>     onVolumeChanged;
    std::function<void(int retIdx, float level)> onSendChanged;
    std::function<void(bool)>      onMuteChanged;
    std::function<void(bool)>      onSoloChanged;
    std::function<void(bool)>      onArmChanged;
    std::function<void(int scene, const juce::String&)> onRenameClipAt;
    std::function<void(int scene, juce::Colour)>        onSetClipColourAt;
    std::function<void(const juce::String&)>            onRenameTrack;
    std::function<void(juce::Colour)>                   onSetTrackColour;
    std::function<void()> onAddScene;

    TrackColumn (int index, const juce::String& name, TrackType type)
        : trackIndex (index)
    {
        header.trackName  = name;
        header.trackType  = type;
        addAndMakeVisible (header);

        slotsViewport.setViewedComponent (&slotsContent, false);
        slotsViewport.setScrollBarsShown (true, false);
        slotsViewport.getVerticalScrollBar().setColour (
            juce::ScrollBar::thumbColourId, juce::Colour (0xff334466));
        addAndMakeVisible (slotsViewport);

        addSceneBtn.setButtonText ("+ Add Pattern");
        addSceneBtn.setTooltip ("Add a clip slot to this track");
        addSceneBtn.setColour (juce::TextButton::buttonColourId,  juce::Colour (0xff0d0d1f));
        addSceneBtn.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff1a1a3a));
        addSceneBtn.setColour (juce::TextButton::textColourOffId,  juce::Colour (0xff3a3a88));
        addSceneBtn.onClick = [this] { if (onAddScene) onAddScene(); };
        slotsContent.addAndMakeVisible (addSceneBtn);

        volFader.setSliderStyle (juce::Slider::LinearVertical);
        volFader.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 50, 16);
        volFader.setRange (0.0, 1.0);
        volFader.setValue (1.0, juce::dontSendNotification);
        volFader.setDoubleClickReturnValue (true, 1.0);
        volFader.onValueChange = [this] {
            if (onVolumeChanged) onVolumeChanged ((float) volFader.getValue());
        };
        addAndMakeVisible (volFader);

        muteBtn.setClickingTogglesState (true);
        styleToggleBtn (muteBtn, juce::Colour (0xffdd8800));
        muteBtn.onClick = [this] {
            isMuted = muteBtn.getToggleState();
            styleToggleBtn (muteBtn, juce::Colour (0xffdd8800));
            if (onMuteChanged) onMuteChanged (isMuted);
        };
        addAndMakeVisible (muteBtn);

        soloBtn.setClickingTogglesState (true);
        styleToggleBtn (soloBtn, juce::Colour (0xff22cc55));
        soloBtn.onClick = [this] {
            isSoloed = soloBtn.getToggleState();
            styleToggleBtn (soloBtn, juce::Colour (0xff22cc55));
            if (onSoloChanged) onSoloChanged (isSoloed);
        };
        addAndMakeVisible (soloBtn);

        recordArmBtn.setClickingTogglesState (true);
        styleToggleBtn (recordArmBtn, juce::Colour (0xffff3333));
        recordArmBtn.onClick = [this] {
            isArmed = recordArmBtn.getToggleState();
            styleToggleBtn (recordArmBtn, juce::Colour (0xffff3333));
            if (onArmChanged) onArmChanged (isArmed);
        };
        addAndMakeVisible (recordArmBtn);
    }

    // -- Add a clip slot ---------------------------------------------------
    void addScene()
    {
        int s      = slots.size();
        auto* slot = new ClipSlot();
        slot->trackIndex  = trackIndex;
        slot->sceneIndex  = s;
        slot->onCreateClip    = [this, s] { if (onCreateClipAt)    onCreateClipAt (s); };
        slot->onSelectClip    = [this, s] { if (onSelectClipAt)    onSelectClipAt (s); };
        slot->onLaunchClip    = [this, s] { if (onLaunchClipAt)    onLaunchClipAt (s); };
        slot->onPauseClip     = [this, s] { if (onPauseClipAt)     onPauseClipAt  (s); };
        slot->onDeleteClip    = [this, s] { if (onDeleteClipAt)    onDeleteClipAt (s); };
        slot->onDuplicateClip = [this, s] { if (onDuplicateClipAt) onDuplicateClipAt (s); };
        slot->onRenameClip    = [this, s] (const juce::String& n) {
            if (onRenameClipAt) onRenameClipAt (s, n); };
        slot->onSetClipColour = [this, s] (juce::Colour c) {
            if (onSetClipColourAt) onSetClipColourAt (s, c); };
        slotsContent.addAndMakeVisible (slot);
        slots.add (slot);
        layoutSlotsContent();
    }

    // -- Add a send knob for a new return ----------------------------------
    void addSendKnob (int retIdx)
    {
        auto* k = new juce::Slider();
        k->setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        k->setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        k->setRange (0.0, 1.0);
        k->setValue (0.0, juce::dontSendNotification);
        k->setDoubleClickReturnValue (true, 0.0);
        k->onValueChange = [this, retIdx] {
            if (onSendChanged) onSendChanged (retIdx, (float) sendKnobs[retIdx]->getValue());
        };
        addAndMakeVisible (k);
        sendKnobs.add (k);
        resized();
        repaint();
    }

    // -- Remove the send knob at retIdx (called when a return track is deleted)
    void removeSendKnob (int retIdx)
    {
        if (juce::isPositiveAndBelow (retIdx, sendKnobs.size()))
        {
            removeChildComponent (sendKnobs[retIdx]);
            sendKnobs.remove (retIdx);
            resized();
            repaint();
        }
    }

    // -- Returns the actual mixer strip height given the current send-knob count
    int computeMixerHeight() const
    {
        static constexpr int kKnobH  = 44;
        static constexpr int kLabelH = 14;
        int h = 4 + 22 + 4
              + sendKnobs.size() * (kLabelH + kKnobH + 2)
              + 4 + 100;
        return juce::jmax (h, SV_MIXER_H);
    }

    // -- Helpers -----------------------------------------------------------
    void setMuted  (bool m) { isMuted  = m; muteBtn.setToggleState (m, juce::dontSendNotification); styleToggleBtn (muteBtn,      juce::Colour (0xffdd8800)); }
    void setSoloed (bool s) { isSoloed = s; soloBtn.setToggleState (s, juce::dontSendNotification); styleToggleBtn (soloBtn,      juce::Colour (0xff22cc55)); }
    void setArmed  (bool a) { isArmed  = a; recordArmBtn.setToggleState (a, juce::dontSendNotification); styleToggleBtn (recordArmBtn, juce::Colour (0xffff3333)); }

    // Layout the inner slotsContent (called by addScene / resized)
    void layoutSlotsContent()
    {
        int vpW = slotsViewport.getWidth();
        int barW = slotsViewport.getScrollBarThickness();
        int w = juce::jmax (4, vpW > 0 ? vpW - barW : getWidth());
        int contentH = slots.size() * SV_SLOT_H + SV_ADD_SCENE_BTN_H;
        contentH = juce::jmax (contentH, slotsViewport.getHeight());
        slotsContent.setSize (w, contentH);
        int y = 0;
        for (auto* sl : slots) { sl->setBounds (2, y, w - 4, SV_SLOT_H - 4); y += SV_SLOT_H; }
        addSceneBtn.setBounds (0, y, w, SV_ADD_SCENE_BTN_H);
    }

    void resized() override
    {
        static constexpr int kKnobH  = 44;
        static constexpr int kLabelH = 14;

        auto b = getLocalBounds();
        header.setBounds (b.removeFromTop (SV_HEADER_H));

        // Mixer strip: fixed at bottom, grows with each send knob row added.
        int mixerH = 4 + 22 + 4
                   + sendKnobs.size() * (kLabelH + kKnobH + 2)
                   + 4 + 100;                 // min-fader height
        mixerH = juce::jmax (mixerH, SV_MIXER_H);
        auto mix = b.removeFromBottom (mixerH).reduced (4);

        auto btnRow = mix.removeFromTop (22);
        int  btnW   = (btnRow.getWidth() - 4) / 3;
        muteBtn.setBounds (btnRow.removeFromLeft (btnW));
        btnRow.removeFromLeft (2);
        soloBtn.setBounds (btnRow.removeFromLeft (btnW));
        btnRow.removeFromLeft (2);
        recordArmBtn.setBounds (btnRow);
        mix.removeFromTop (4);

        for (auto* k : sendKnobs)
        {
            mix.removeFromTop (kLabelH);
            k->setBounds (mix.removeFromTop (kKnobH).withSizeKeepingCentre (44, 44));
            mix.removeFromTop (2);
        }
        volFader.setBounds (mix);

        // Clip viewport fills the rest between header and mixer strip
        slotsViewport.setBounds (b);
        layoutSlotsContent();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (isSelected ? juce::Colour (0xff1a1a2a) : juce::Colour (0xff111120));
        g.setColour (isSelected ? juce::Colour (0xff555577) : juce::Colour (0xff252540));
        g.drawRect (getLocalBounds(), isSelected ? 2 : 1);

        if (sendKnobs.isEmpty()) return;
        static constexpr int kKnobH  = 44;
        static constexpr int kLabelH = 14;
        g.setColour (juce::Colour (0xff8888aa));
        g.setFont (juce::Font (juce::FontOptions (9.5f, juce::Font::bold)));
        int labelY = sendKnobs[0]->getY() - kLabelH;
        for (int i = 0; i < sendKnobs.size(); ++i)
        {
            juce::String lbl = "SEND "; lbl += (char)('A' + i);
            g.drawText (lbl, 0, labelY, getWidth(), kLabelH, juce::Justification::centred);
            labelY += kLabelH + kKnobH + 2;
        }
    }

    void paintOverChildren (juce::Graphics& g) override
    {
        auto fb = volFader.getBounds();
        static constexpr int kTextBoxH = 16;
        const int   trackTop    = fb.getY();
        const int   trackBottom = fb.getBottom() - kTextBoxH;
        const float trackH      = (float)(trackBottom - trackTop);
        if (trackH < 4.0f) return;

        if (gainValue > 0.01f)
        {
            float gainNorm = juce::jlimit (0.0f, 1.0f, gainValue);
            float thumbY   = trackTop + trackH * (1.0f - gainNorm);
            juce::Rectangle<float> gainBar ((float)fb.getCentreX() - 2.0f, thumbY, 4.0f, (float)trackBottom - thumbY);
            juce::ColourGradient grad (juce::Colour (0xffcc8833).withAlpha (0.45f), gainBar.getX(), gainBar.getY(),
                                       juce::Colour (0xff995500).withAlpha (0.10f), gainBar.getX(), gainBar.getBottom(), false);
            g.setGradientFill (grad);
            g.fillRoundedRectangle (gainBar, 2.0f);
        }

        if (rmsLevel > 0.002f)
        {
            float lvl    = juce::jlimit (0.0f, 1.0f, rmsLevel);
            float meterH = trackH * lvl;
            float mw     = 5.0f;
            float mx     = (float)fb.getCentreX() - mw * 0.5f;
            float my     = (float)trackBottom - meterH;
            juce::Colour meterCol = lvl > 0.85f ? juce::Colour (0xffff4444)
                                  : lvl > 0.65f ? juce::Colour (0xffddcc00)
                                                : juce::Colour (0xff22dd77);
            juce::ColourGradient mGrad (meterCol.withAlpha (0.90f), mx, my,
                                        meterCol.withAlpha (0.30f), mx, (float)trackBottom, false);
            g.setGradientFill (mGrad);
            g.fillRoundedRectangle (mx, my, mw, meterH, 2.0f);
        }
    }

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

    void setPlayhead (float phase)
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
        volFader.setValue (1.0, juce::dontSendNotification);
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
        int faderTop = SV_HEADER_H;
        repaint (0, faderTop, getWidth(), getHeight() - faderTop);
    }

    std::function<void()> onSelectTrack;
    std::function<void()> onDeleteTrack;
    // Group-specific (only used when isGroupColumn == true)
    bool isGroupColumn = false;
    std::function<void(const juce::String&)> onRenameGroup;
    std::function<void()>                    onUngroupAll;

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
        {
            juce::PopupMenu m;
            if (isGroupColumn)
            {
                m.addItem (1, "Rename Group");
                m.addItem (2, "Delete Group");
                m.addSeparator();
                m.addItem (3, "Ungroup All");
                m.showMenuAsync (juce::PopupMenu::Options(), [this](int result) {
                    if (result == 1 && onRenameGroup)
                    {
                        auto* box = new juce::AlertWindow ("Rename Group", "Enter new name:",
                                                          juce::AlertWindow::NoIcon);
                        box->addTextEditor ("name", trackName);
                        box->addButton ("OK",     1, juce::KeyPress (juce::KeyPress::returnKey));
                        box->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
                        box->enterModalState (true,
                            juce::ModalCallbackFunction::create ([this, box](int r) {
                                if (r == 1)
                                {
                                    juce::String n = box->getTextEditorContents ("name").trim();
                                    if (n.isNotEmpty()) { trackName = n; repaint(); if (onRenameGroup) onRenameGroup (n); }
                                }
                                delete box;
                            }), true);
                    }
                    else if (result == 2 && onDeleteTrack) onDeleteTrack();
                    else if (result == 3 && onUngroupAll) onUngroupAll();
                });
            }
            else
            {
                m.addItem (1, "Delete Return Track");
                m.showMenuAsync (juce::PopupMenu::Options(), [this](int result) {
                    if (result == 1 && onDeleteTrack) onDeleteTrack();
                });
            }
            return;
        }
        if (onSelectTrack) onSelectTrack();
    }

    void resized() override
    {
        auto b = getLocalBounds();
        b.removeFromTop (SV_HEADER_H);
        volFader.setBounds (b.reduced (6));
    }
};
// \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500
// GroupColumn  \u2013  Inline group bus / folder track (Ableton-style)
// \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500
class GroupColumn : public juce::Component
{
public:
    int          groupIndex = -1;
    bool         folded     = false;
    int          childCount = 0;
    juce::String groupName;
    juce::Colour groupColour { juce::Colour(0xff44aa88) };
    bool         isSelected = false;
    float        rmsLevel   = 0.0f;
    juce::Slider volFader;

    std::function<void()>                    onToggleFold;
    std::function<void()>                    onDeleteGroup;
    std::function<void()>                    onUngroupAll;
    std::function<void(const juce::String&)> onRenameGroup;
    std::function<void(float)>               onVolumeChanged;
    std::function<void()>                    onSelectGroup;

    explicit GroupColumn (int idx, const juce::String& name, juce::Colour col)
        : groupIndex(idx), groupName(name), groupColour(col)
    {
        volFader.setSliderStyle (juce::Slider::LinearVertical);
        volFader.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 50, 16);
        volFader.setRange (0.0, 1.0);
        volFader.setValue (1.0, juce::dontSendNotification);
        volFader.setDoubleClickReturnValue (true, 1.0);
        volFader.onValueChange = [this] {
            if (onVolumeChanged) onVolumeChanged ((float) volFader.getValue());
        };
        addAndMakeVisible (volFader);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (isSelected ? juce::Colour(0xff1a2420) : juce::Colour(0xff131a18));
        g.setColour (groupColour);
        g.fillRect (0, 0, 6, getHeight());
        g.setColour (juce::Colour(0xff252540));
        g.drawHorizontalLine (SV_HEADER_H, 6.0f, (float)getWidth());

        // Fold toggle button
        auto btnR = juce::Rectangle<float>(8.0f, (SV_HEADER_H - 16) * 0.5f, 16.0f, 16.0f);
        g.setColour (groupColour.withAlpha(0.25f));
        g.fillRoundedRectangle (btnR, 3.0f);
        g.setColour (groupColour);
        {
            juce::Path arrow;
            float ax = btnR.getCentreX(), ay = btnR.getCentreY();
            if (folded)
                arrow.addTriangle (ax - 4, ay - 5, ax - 4, ay + 5, ax + 5, ay);
            else
                arrow.addTriangle (ax - 5, ay - 3, ax + 5, ay - 3, ax, ay + 5);
            g.fillPath (arrow);
        }

        // "G" badge
        auto bBox = juce::Rectangle<int>(28, (SV_HEADER_H - 16) / 2, 16, 16);
        g.setColour (groupColour.withAlpha(0.25f));
        g.fillRoundedRectangle (bBox.toFloat(), 3.0f);
        g.setColour (groupColour);
        g.setFont (juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
        g.drawText ("G", bBox, juce::Justification::centred);

        // Group name
        g.setColour (isSelected ? juce::Colours::white : juce::Colours::lightgrey);
        g.setFont (juce::Font(juce::FontOptions(12.0f, juce::Font::bold)));
        g.drawText (groupName, 48, 0, getWidth() - 52, SV_HEADER_H, juce::Justification::centredLeft);

        // Middle slot area: coloured band + label
        int slotTop = SV_HEADER_H;
        int slotH   = getHeight() - SV_HEADER_H - SV_MIXER_H;
        if (slotH > 0)
        {
            g.setColour (groupColour.withAlpha(0.10f));
            g.fillRect (6, slotTop, getWidth() - 6, slotH);
            g.setColour (groupColour.withAlpha(0.45f));
            g.setFont (juce::Font(juce::FontOptions(10.0f)));
            juce::String lbl = childCount > 0
                ? juce::String(childCount) + (childCount != 1 ? " tracks" : " track")
                    + (folded ? "  \u25b6" : "  \u25bc")
                : "empty";
            g.drawText (lbl, 6, slotTop, getWidth() - 12, slotH, juce::Justification::centred);
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        auto btnR = juce::Rectangle<int>(8, (SV_HEADER_H - 16) / 2, 16, 16);
        if (e.getPosition().y < SV_HEADER_H && btnR.contains(e.getPosition()) && !e.mods.isPopupMenu())
            { if (onToggleFold) onToggleFold(); return; }

        if (e.mods.isPopupMenu())
        {
            juce::PopupMenu m;
            m.addItem (1, "Rename Group");
            m.addItem (2, "Delete Group");
            m.addSeparator();
            m.addItem (3, "Ungroup All");
            m.showMenuAsync (juce::PopupMenu::Options(), [this](int result) {
                if (result == 1)
                {
                    auto* box = new juce::AlertWindow ("Rename Group", "Enter new name:", juce::AlertWindow::NoIcon);
                    box->addTextEditor ("name", groupName);
                    box->addButton ("OK",     1, juce::KeyPress(juce::KeyPress::returnKey));
                    box->addButton ("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
                    box->enterModalState (true, juce::ModalCallbackFunction::create([this, box](int r) {
                        if (r == 1) {
                            juce::String n = box->getTextEditorContents("name").trim();
                            if (n.isNotEmpty()) { groupName = n; repaint(); if (onRenameGroup) onRenameGroup(n); }
                        }
                        delete box;
                    }), true);
                }
                else if (result == 2 && onDeleteGroup) onDeleteGroup();
                else if (result == 3 && onUngroupAll)  onUngroupAll();
            });
            return;
        }
        if (onSelectGroup) onSelectGroup();
    }

    void resized() override
    {
        int mixerTop = getHeight() - SV_MIXER_H;
        volFader.setBounds (juce::Rectangle<int>(0, mixerTop, getWidth(), SV_MIXER_H).reduced(6));
    }
};



// ─────────────────────────────────────────────────────────────────────────────
// TrackGridContent  –  the scrollable inner layer
// ─────────────────────────────────────────────────────────────────────────────
class TrackGridContent : public juce::Component
{
public:
    juce::OwnedArray<TrackColumn> columns;
    juce::OwnedArray<GroupColumn> groupCols;  // inline group bus columns
    bool isDragOver = false; // controlled by SessionView DnD

    // \u2500\u2500 Display Order (Ableton-style folder model) \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500
    struct DisplayEntry {
        bool isGroup;       // true = GroupColumn, false = audio TrackColumn
        int  engineIdx;     // index into groupCols[] or columns[]
        int  parentGroup;   // -1 if top-level or IS a group
    };
    std::vector<DisplayEntry> displayOrder;
    std::vector<int>          trackParentGroup;  // per audio track, -1 = ungrouped
    bool                      groupFolded[8] = {};

    // Group-level callbacks (forwarded to SessionView)
    std::function<void(int groupIdx, float gain)>    onGroupVolumeChanged;
    std::function<void(int groupIdx)>                onDeleteGroupInGrid;
    std::function<void(int groupIdx)>                onUngroupAllInGrid;
    std::function<void(int groupIdx, const juce::String&)> onRenameGroupInGrid;
    std::function<void(int groupIdx)>                onToggleGroupFold;
    std::function<void(int groupIdx)>                onSelectGroup;


    std::function<void(int track, int scene)>                          onCreateClip;
    std::function<void(int track, int scene)>                          onSelectClip;
    std::function<void(int track, int scene)>                          onLaunchClip;
    std::function<void(int track, int scene)>                          onPauseClip;
    std::function<void(int track, int scene)>                          onDeleteClip;
    std::function<void(int track, int scene)>                          onDuplicateClip;
    std::function<void(int trackIndex)>                                onSelectTrack;
    std::function<void(int trackIndex)>                                onDeleteTrack;
    std::function<void(int trackIndex, bool soloed)>                   onTrackSoloChanged;
    std::function<void(int trackIndex, bool armed)>                    onTrackArmChanged;
    std::function<void(int trackIndex, const juce::String& type)>      onInstrumentDropped;
    std::function<void(int trackIndex, const juce::String& type)>      onEffectDropped;
    std::function<void(int trackIndex, float gain)>                    onTrackVolumeChanged;
    // trackIndex, retIdx (0=RetA, 1=RetB…), send level
    std::function<void(int trackIndex, int retIdx, float level)>       onTrackSendChanged;
    std::function<void(int trackIndex, bool muted)>                    onTrackMuteChanged;
    std::function<void(int track, int scene, const juce::String&)>     onRenameClip;
    std::function<void(int track, int scene, juce::Colour)>            onSetClipColour;
    std::function<void(int trackIndex, const juce::String&)>           onRenameTrack;
    std::function<void(int trackIndex, juce::Colour)>                  onSetTrackColour;
    std::function<void(int trackIndex)>                                onAddScene;
    // Group routing
    std::function<juce::StringArray()>                                 getGroupNames;  // for submenu
    std::function<void(int trackIdx, int groupIdx)>                    onAssignToGroup;

    // Tracks the number of active return buses so that new columns get the right number of knobs.
    int numReturnTracksCached = 0;
    // ── Track Management ─────────────────────────────────────────────────────
    int addTrack (TrackType type, const juce::String& name)
    {
        int idx  = columns.size();
        auto* col = new TrackColumn (idx, name, type);
        col->onCreateClipAt = [this, idx] (int s) { if (onCreateClip) onCreateClip (idx, s); };
        col->onSelectClipAt = [this, idx] (int s) { if (onSelectClip) onSelectClip (idx, s); };
        col->onLaunchClipAt = [this, idx] (int s) { if (onLaunchClip) onLaunchClip (idx, s); };
        col->onPauseClipAt  = [this, idx] (int s) { if (onPauseClip)  onPauseClip  (idx, s); };
        col->onDeleteClipAt = [this, idx] (int s) { if (onDeleteClip) onDeleteClip (idx, s); };
        col->onDuplicateClipAt = [this, idx] (int s) { if (onDuplicateClip) onDuplicateClip (idx, s); };
        col->onSelectTrack  = [this, idx] () { if (onSelectTrack) onSelectTrack (idx); };
        col->onDeleteTrack  = [this, idx] () { if (onDeleteTrack) onDeleteTrack (idx); };
        col->onVolumeChanged = [this, idx] (float g) { if (onTrackVolumeChanged) onTrackVolumeChanged (idx, g); };
        col->onSendChanged   = [this, idx] (int r, float l) { if (onTrackSendChanged) onTrackSendChanged (idx, r, l); };
        col->onMuteChanged      = [this, idx] (bool m)  { if (onTrackMuteChanged)   onTrackMuteChanged   (idx, m); };
        col->onSoloChanged      = [this, idx] (bool s)  { if (onTrackSoloChanged)   onTrackSoloChanged   (idx, s); };
        col->onArmChanged       = [this, idx] (bool a)  { if (onTrackArmChanged)    onTrackArmChanged    (idx, a); };
        col->onRenameTrack      = [this, idx] (const juce::String& n) { if (onRenameTrack)    onRenameTrack    (idx, n); };
        col->onSetTrackColour   = [this, idx] (juce::Colour c)         { if (onSetTrackColour) onSetTrackColour (idx, c); };
        col->onRenameClipAt     = [this, idx] (int s, const juce::String& n) { if (onRenameClip)    onRenameClip    (idx, s, n); };
        col->onSetClipColourAt  = [this, idx] (int s, juce::Colour c)         { if (onSetClipColour) onSetClipColour (idx, s, c); };
        col->onAddScene         = [this, idx] () { if (onAddScene) onAddScene (idx); };
        col->header.onSelectTrack   = col->onSelectTrack;
        col->header.onDeleteTrack   = col->onDeleteTrack;
        col->header.onRenameTrack   = col->onRenameTrack;
        col->header.onSetTrackColour = col->onSetTrackColour;
        col->header.getGroupNames   = [this]() -> juce::StringArray {
            return getGroupNames ? getGroupNames() : juce::StringArray();
        };
        col->header.onAssignToGroup = [this, idx](int groupIdx) {
            if (onAssignToGroup) onAssignToGroup(idx, groupIdx);
        };
        // Provision one send knob per existing return track
        for (int r = 0; r < numReturnTracksCached; ++r)
            col->addSendKnob(r);
        addAndMakeVisible (col);
        columns.add (col);
        // Explicitly re-layout: setSize() in updateContentSize() may not change the
        // total size (if the viewport is wide enough), so resized() would never fire
        // and the new column would get zero bounds. Always force the layout here.
        trackParentGroup.push_back(-1);
        rebuildDisplayOrder();
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
            if (index < (int)trackParentGroup.size())
                trackParentGroup.erase(trackParentGroup.begin() + index);
            columns.remove (index);
            for (int i = index; i < columns.size(); ++i)
            {
                columns[i]->trackIndex = i;
                columns[i]->onCreateClipAt = [this, i] (int s) { if (onCreateClip) onCreateClip (i, s); };
                columns[i]->onSelectClipAt = [this, i] (int s) { if (onSelectClip) onSelectClip (i, s); };
                columns[i]->onDeleteClipAt = [this, i] (int s) { if (onDeleteClip) onDeleteClip (i, s); };
                columns[i]->onPauseClipAt  = [this, i] (int s) { if (onPauseClip)  onPauseClip  (i, s); };
                columns[i]->onDuplicateClipAt = [this, i] (int s) { if (onDuplicateClip) onDuplicateClip (i, s); };
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
                    sl->onPauseClip  = [this, i, s = sl->sceneIndex] { if (onPauseClip)  onPauseClip  (i, s); };
                    sl->onDeleteClip = [this, i, s = sl->sceneIndex] { if (onDeleteClip) onDeleteClip (i, s); };
                    sl->onDuplicateClip = [this, i, s = sl->sceneIndex] { if (onDuplicateClip) onDuplicateClip (i, s); };
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
        for (auto* c : columns)   c->setVisible(false);
        for (auto* g : groupCols) g->setVisible(false);
        int x = 0;
        for (auto& e : displayOrder)
        {
            bool childHidden = !e.isGroup && e.parentGroup >= 0 && e.parentGroup < 8 && groupFolded[e.parentGroup];
            if (e.isGroup) { auto* gc = groupCols[e.engineIdx]; gc->setBounds(x, 0, SV_TRACK_W, h); gc->setVisible(true); x += SV_TRACK_W; }
            else if (!childHidden) { auto* col = columns[e.engineIdx]; col->setBounds(x, 0, SV_TRACK_W, h); col->setVisible(true); x += SV_TRACK_W; }
        }
    }

    void rebuildDisplayOrder()
    {
        displayOrder.clear();
        int nG = groupCols.size(), nT = columns.size();
        for (int g = 0; g < nG; ++g)
        {
            displayOrder.push_back({ true, g, -1 });
            for (int t = 0; t < nT; ++t)
                if (t < (int)trackParentGroup.size() && trackParentGroup[t] == g)
                    displayOrder.push_back({ false, t, g });
        }
        for (int t = 0; t < nT; ++t)
            if (t >= (int)trackParentGroup.size() || trackParentGroup[t] < 0)
                displayOrder.push_back({ false, t, -1 });
        for (int g = 0; g < nG; ++g)
        {
            int cnt = 0;
            for (int t = 0; t < nT; ++t)
                if (t < (int)trackParentGroup.size() && trackParentGroup[t] == g) ++cnt;
            groupCols[g]->childCount = cnt; groupCols[g]->repaint();
        }
    }

    void setTrackParent(int trackIdx, int groupIdx)
    {
        if (juce::isPositiveAndBelow(trackIdx, (int)trackParentGroup.size()))
            trackParentGroup[trackIdx] = groupIdx;
        rebuildDisplayOrder(); resized(); repaint();
    }

    void setGroupFolded(int groupIdx, bool folded)
    {
        if (groupIdx < 0 || groupIdx >= 8) return;
        groupFolded[groupIdx] = folded;
        if (juce::isPositiveAndBelow(groupIdx, groupCols.size()))
            { groupCols[groupIdx]->folded = folded; groupCols[groupIdx]->repaint(); }
        resized(); repaint();
    }

    int visibleColumnCount() const
    {
        int n = 0;
        for (auto& e : displayOrder)
        {
            if (e.isGroup) { ++n; continue; }
            if (e.parentGroup >= 0 && e.parentGroup < 8 && groupFolded[e.parentGroup]) continue;
            ++n;
        }
        return n;
    }

    int addGroupCol(int engineGroupIdx, const juce::String& name, juce::Colour col)
    {
        auto* gc = new GroupColumn(engineGroupIdx, name, col);
        gc->onToggleFold   = [this, engineGroupIdx]() { setGroupFolded(engineGroupIdx, !groupFolded[engineGroupIdx]); if (onToggleGroupFold) onToggleGroupFold(engineGroupIdx); };
        gc->onDeleteGroup  = [this, engineGroupIdx]() { if (onDeleteGroupInGrid) onDeleteGroupInGrid(engineGroupIdx); };
        gc->onUngroupAll   = [this, engineGroupIdx]() { if (onUngroupAllInGrid) onUngroupAllInGrid(engineGroupIdx); };
        gc->onRenameGroup  = [this, engineGroupIdx](const juce::String& n) { if (onRenameGroupInGrid) onRenameGroupInGrid(engineGroupIdx, n); };
        gc->onVolumeChanged= [this, engineGroupIdx](float v) { if (onGroupVolumeChanged) onGroupVolumeChanged(engineGroupIdx, v); };
        gc->onSelectGroup  = [this, engineGroupIdx]() { if (onSelectGroup) onSelectGroup(engineGroupIdx); };
        addAndMakeVisible(gc); groupCols.add(gc);
        rebuildDisplayOrder(); resized(); repaint();
        return groupCols.size() - 1;
    }

    void removeGroupCol(int engineGroupIdx)
    {
        if (!juce::isPositiveAndBelow(engineGroupIdx, groupCols.size())) return;
        for (auto& pg : trackParentGroup) { if (pg == engineGroupIdx) pg = -1; else if (pg > engineGroupIdx) --pg; }
        for (int i = engineGroupIdx; i < 7; ++i) groupFolded[i] = groupFolded[i + 1];
        groupFolded[7] = false;
        for (int g = engineGroupIdx; g < groupCols.size() - 1; ++g)
        {
            int ng = g;
            groupCols[g + 1]->groupIndex    = g;
            groupCols[g + 1]->onToggleFold  = [this,ng]() { setGroupFolded(ng,!groupFolded[ng]); if (onToggleGroupFold) onToggleGroupFold(ng); };
            groupCols[g + 1]->onDeleteGroup = [this,ng]() { if (onDeleteGroupInGrid) onDeleteGroupInGrid(ng); };
            groupCols[g + 1]->onUngroupAll  = [this,ng]() { if (onUngroupAllInGrid)  onUngroupAllInGrid(ng); };
            groupCols[g + 1]->onRenameGroup = [this,ng](const juce::String& n) { if (onRenameGroupInGrid) onRenameGroupInGrid(ng,n); };
            groupCols[g + 1]->onVolumeChanged= [this,ng](float v) { if (onGroupVolumeChanged) onGroupVolumeChanged(ng,v); };
        }
        groupCols.remove(engineGroupIdx);
        rebuildDisplayOrder(); resized(); repaint();
    }


    // Add one send knob to every existing column (called after a new return track is created)
    void addSendKnobToAllColumns (int retIdx)
    {
        for (auto* col : columns)
            col->addSendKnob (retIdx);
        numReturnTracksCached = retIdx + 1;
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff0e0e1c));

        // Row tints omitted: each column has its own independent slot count.

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
    std::function<void(int track, int scene)> onPauseClip;
    std::function<void(int track, int scene)> onDeleteClip;
    std::function<void(int track, int scene)> onDuplicateClip;
    std::function<void(int track)>             onSelectTrack;
    std::function<void(int track)>             onDeleteTrack;
    std::function<void(int sceneIndex)>        onLaunchScene;
    std::function<void(int trackIndex, const juce::String& instrumentType)> onInstrumentDropped;
    std::function<void(int trackIndex, const juce::String& effectType)>     onEffectDropped;
    // Called by the effect-drop path when no column hit is found (e.g. in Arrangement mode).
    // Should return the currently selected track index, or -1 if nothing is selected.
    std::function<int()> getSelectedTrackIndex;
    std::function<void(int trackIndex, float gain)> onTrackVolumeChanged;
    // trackIndex, retIdx (0=RetA, 1=RetB…), send level
    std::function<void(int trackIndex, int retIdx, float level)> onTrackSendChanged;
    std::function<void(float gain)>                 onMasterVolumeChanged;
    // Fired when a ReturnRackDrag is dropped onto a track; the listener creates a new return bus.
    std::function<void()>                           onReturnRackDropped;
    std::function<void(int retIdx, float gain)>     onReturnVolumeChanged;
    std::function<void(int retIdx)>                 onDeleteReturnTrack;
    std::function<void(int trackIndex, bool muted)>  onTrackMuteChanged;
    std::function<void(int trackIndex, bool soloed)> onTrackSoloChanged;
    std::function<void(int trackIndex, bool armed)>  onTrackArmChanged;
    std::function<void(int track, int scene, const juce::String&)> onRenameClip;
    std::function<void(int track, int scene, juce::Colour)>        onSetClipColour;
    std::function<void(int trackIndex, const juce::String&)>       onRenameTrack;
    std::function<void(int trackIndex, juce::Colour)>              onSetTrackColour;
    std::function<void()>                                          onSceneLabelClicked;
    std::function<void(int trackIndex)> onAddScene;

    // ── Group Track Callbacks (2.2) ─────────────────────────────────────────────────
    std::function<void()>                        onAddGroupTrack;
    std::function<void(int groupIdx)>            onDeleteGroupTrack;
    // trackIdx, groupIdx (-1 = route to master)
    std::function<void(int trackIdx, int groupIdx)> onRouteTrackToGroup;
    std::function<void(int groupIdx, float gain)>   onGroupVolumeChanged;
    // trackIdx, groupIdx (-1 = remove from group)
    std::function<void(int trackIdx, int groupIdx)> onAssignToGroup;
    // groupIdx to clear all members
    std::function<void(int groupIdx)>               onUngroupAll;
    // groupIdx, new name
    std::function<void(int groupIdx, const juce::String&)> onRenameGroupTrack;

    // Update the group badge on an audio track header column.
    // groupIdx == -1 clears the badge.
    void setTrackGroupBadge (int trackIdx, int groupIdx,
                             const juce::String& groupName, juce::Colour groupColour)
    {
        if (!juce::isPositiveAndBelow (trackIdx, gridContent.columns.size())) return;
        auto& hdr = gridContent.columns[trackIdx]->header;
        if (groupIdx < 0) {
            hdr.groupBadgeText = {};
        } else {
            hdr.groupBadgeText   = groupName.isEmpty() ? ("G" + juce::String(groupIdx + 1)) : groupName;
            hdr.groupBadgeColour = groupColour;
        }
        hdr.repaint();
    }

    // Build the dynamic group names list used by the TrackHeader "Assign to Group" submenu.
    // Returns: [ "No Group", "Group 1", "Group 2", ..., "New Group..." ]
    juce::StringArray buildGroupNamesList() const
    {
        juce::StringArray names;
        names.add ("No Group");
        for (int i = 0; i < gridContent.groupCols.size(); ++i)
            names.add (gridContent.groupCols[i]->groupName);
        names.add ("New Group\u2026");
        return names;
    }

    // ── Sidechain Source Callback (2.1) ────────────────────────────────────────────────
    // targetTrackIdx, sourceTrackIdx (-1 = clear sidechain)
    std::function<void(int targetTrackIdx, int sourceTrackIdx)> onSidechainSourceChanged;

    SessionView()
    {
        // Wire grid content → session callbacks (forward pattern)
        gridContent.onCreateClip = [this] (int t, int s) { if (onCreateClip)         onCreateClip (t, s); };
        gridContent.onSelectClip = [this] (int t, int s) { if (onSelectClip)         onSelectClip (t, s); };
        gridContent.onLaunchClip = [this] (int t, int s) { if (onLaunchClip)         onLaunchClip (t, s); };
        gridContent.onPauseClip  = [this] (int t, int s) { if (onPauseClip)          onPauseClip  (t, s); };
        gridContent.onDeleteClip = [this] (int t, int s) { if (onDeleteClip)         onDeleteClip (t, s); };
        gridContent.onDuplicateClip = [this] (int t, int s) { if (onDuplicateClip)   onDuplicateClip(t, s); };
        gridContent.onSelectTrack = [this] (int t)       { if (onSelectTrack)        onSelectTrack(t); };
        gridContent.onDeleteTrack = [this] (int t)       { if (onDeleteTrack)        onDeleteTrack(t); };
        gridContent.onTrackSoloChanged = [this] (int t, bool s) { if (onTrackSoloChanged) onTrackSoloChanged (t, s); };
        gridContent.onTrackArmChanged  = [this] (int t, bool a) { if (onTrackArmChanged)  onTrackArmChanged  (t, a); };
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
        gridContent.onTrackSendChanged = [this] (int t, int r, float l)
        {
            if (onTrackSendChanged) onTrackSendChanged (t, r, l);
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
        gridContent.onAddScene = [this] (int t) { if (onAddScene) onAddScene (t); };
        gridContent.getGroupNames   = [this]() { return buildGroupNamesList(); };
        gridContent.onAssignToGroup = [this] (int t, int g) { if (onAssignToGroup) onAssignToGroup (t, g); };
        gridContent.onGroupVolumeChanged = [this](int g, float v) { if (onGroupVolumeChanged) onGroupVolumeChanged(g, v); };
        gridContent.onDeleteGroupInGrid  = [this](int g) { deleteGroupTrackInline(g); if (onDeleteGroupTrack) onDeleteGroupTrack(g); };
        gridContent.onUngroupAllInGrid   = [this](int g) { if (onUngroupAll) onUngroupAll(g); };
        gridContent.onRenameGroupInGrid  = [this](int g, const juce::String& n) { if (onRenameGroupTrack) onRenameGroupTrack(g, n); };
        gridContent.onToggleGroupFold    = [this](int g) { /* fold state already handled in gridContent */ };
        gridContent.onSelectGroup        = [this](int g) { if (onSelectTrack) onSelectTrack(2000 + g); };

        // Viewport – horizontal scroll only
        gridViewport.setViewedComponent (&gridContent, false);
        gridViewport.setScrollBarsShown (false, true);
        gridViewport.getHorizontalScrollBar().setColour (
            juce::ScrollBar::thumbColourId, juce::Colour (0xff334466));
        addAndMakeVisible (gridViewport);

        // Scene launch buttons are added dynamically via addSceneButton().

        // Scene launch buttons are added dynamically via addSceneButton().

        masterColumn = std::make_unique<FixedTrackColumn> ("Master");
        masterColumn->onVolumeChanged = [this] (float g) { if (onMasterVolumeChanged) onMasterVolumeChanged (g); };
        
        addReturnTrack("Return A");
        
        addAndMakeVisible (masterColumn.get());
    }

    // ── Public API ────────────────────────────────────────────────────────────
    int addTrack (TrackType type, const juce::String& name)
    {
        int idx = gridContent.addTrack (type, name);
        updateContentSize();
        return idx;
    }

    void addSceneToTrack (int trackIndex)
    {
        if (juce::isPositiveAndBelow (trackIndex, gridContent.columns.size()))
        {
            gridContent.columns[trackIndex]->addScene();
            updateContentSize();
            gridContent.resized();
            gridContent.columns[trackIndex]->resized();
            updateSceneButtons();
        }
    }
    
    void addReturnTrack(const juce::String& name)
    {
        int retIdx = (int) returnColumns.size();
        auto col = std::make_unique<FixedTrackColumn>(name);
        col->onVolumeChanged = [this, retIdx](float g) {
            if (onReturnVolumeChanged) onReturnVolumeChanged(retIdx, g);
        };
        col->onSelectTrack = [this, retIdx]() {
            if (onSelectTrack) onSelectTrack(1000 + retIdx);
        };
        col->onDeleteTrack = [this, retIdx]() {
            deleteReturnTrack(retIdx);
        };
        addAndMakeVisible(col.get());
        returnColumns.push_back(std::move(col));
        // Keep numReturnTracksCached in sync so new track columns created
        // after this point are provisioned with the correct number of send knobs.
        gridContent.numReturnTracksCached = (int) returnColumns.size();
        resized();
        repaint();
    }

    // Fired after the engine side has already removed the return track.
    // Removes the column and the matching send knob from every track column.
    void deleteReturnTrack (int retIdx)
    {
        if (!juce::isPositiveAndBelow (retIdx, (int) returnColumns.size())) return;

        // Remove the FixedTrackColumn from the view
        removeChildComponent (returnColumns[retIdx].get());
        returnColumns.erase (returnColumns.begin() + retIdx);

        // Remove the matching send knob from every regular track column
        for (auto* col : gridContent.columns)
            col->removeSendKnob (retIdx);

        gridContent.numReturnTracksCached = (int) returnColumns.size();

        // Re-wire onDeleteTrack closures for columns that shifted down
        for (int i = retIdx; i < (int) returnColumns.size(); ++i)
        {
            int newIdx = i;
            returnColumns[i]->onVolumeChanged = [this, newIdx](float g) {
                if (onReturnVolumeChanged) onReturnVolumeChanged(newIdx, g);
            };
            returnColumns[i]->onSelectTrack = [this, newIdx]() {
                if (onSelectTrack) onSelectTrack(1000 + newIdx);
            };
            returnColumns[i]->onDeleteTrack = [this, newIdx]() {
                deleteReturnTrack(newIdx);
            };
        }

        if (onDeleteReturnTrack) onDeleteReturnTrack (retIdx);

        resized();
        repaint();
    }

    // ── Group Track UI Methods (2.2) ─────────────────────────────────────────
    // \u2500\u2500 Inline group track API (Ableton-style) \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500
    static const juce::Colour kGroupColours[8];

    void addGroupTrackInline (int engineGroupIdx, const juce::String& name)
    {
        static const juce::Colour pal[8] = {
            juce::Colour(0xff44aa88), juce::Colour(0xff8844aa),
            juce::Colour(0xffaa8844), juce::Colour(0xff4488aa),
            juce::Colour(0xffaa4488), juce::Colour(0xff88aa44),
            juce::Colour(0xff4444aa), juce::Colour(0xffaa4444),
        };
        juce::Colour col = pal[engineGroupIdx % 8];
        gridContent.addGroupCol(engineGroupIdx, name, col);
        updateContentSize();
    }

    void deleteGroupTrackInline (int engineGroupIdx)
    {
        gridContent.removeGroupCol(engineGroupIdx);
        if (onDeleteGroupTrack) onDeleteGroupTrack(engineGroupIdx);
        updateContentSize();
    }

    void setTrackParentGroup (int trackIdx, int groupIdx)
    {
        gridContent.setTrackParent(trackIdx, groupIdx);
        updateContentSize();
    }

    void setGroupFolded (int groupIdx, bool folded)
    {
        gridContent.setGroupFolded(groupIdx, folded);
        updateContentSize();
    }

    // Legacy stub kept for backwards compatibility with existing call sites.
    // Routes to inline implementation.
    void addGroupTrack (const juce::String& name)
    {
        addGroupTrackInline((int)groupColumns.size(), name);
    }
    void deleteGroupTrack (int grpIdx)
    {
        deleteGroupTrackInline(grpIdx);
    }

    // Remove scene slot at sceneIndex from the given track column,
    // compact the slot list, and re-wire scene indices.
    void removeSceneFromTrack (int trackIndex, int sceneIndex)
    {
        if (!juce::isPositiveAndBelow (trackIndex, gridContent.columns.size())) return;
        auto* col = gridContent.columns[trackIndex];
        if (!juce::isPositiveAndBelow (sceneIndex, col->slots.size())) return;

        // Remove the slot component
        col->removeChildComponent (col->slots[sceneIndex]);
        col->slots.remove (sceneIndex, true); // deletes the object

        // Re-wire scene indices and callbacks for all subsequent slots
        for (int si = sceneIndex; si < col->slots.size(); ++si)
        {
            col->slots[si]->sceneIndex = si;
            col->slots[si]->onCreateClip    = [col, si] { if (col->onCreateClipAt)    col->onCreateClipAt (si); };
            col->slots[si]->onSelectClip    = [col, si] { if (col->onSelectClipAt)    col->onSelectClipAt (si); };
            col->slots[si]->onLaunchClip    = [col, si] { if (col->onLaunchClipAt)    col->onLaunchClipAt (si); };
            col->slots[si]->onPauseClip     = [col, si] { if (col->onPauseClipAt)     col->onPauseClipAt  (si); };
            col->slots[si]->onDeleteClip    = [col, si] { if (col->onDeleteClipAt)    col->onDeleteClipAt (si); };
            col->slots[si]->onDuplicateClip = [col, si] { if (col->onDuplicateClipAt) col->onDuplicateClipAt (si); };
            col->slots[si]->onRenameClip    = [col, si] (const juce::String& n) { if (col->onRenameClipAt)    col->onRenameClipAt    (si, n); };
            col->slots[si]->onSetClipColour = [col, si] (juce::Colour c)         { if (col->onSetClipColourAt) col->onSetClipColourAt (si, c); };
        }

        updateContentSize();
        gridContent.resized();
        col->resized();
        updateSceneButtons();
    }

    // Sync the scene-launch button strip to the maximum slot count across all tracks.
    void updateSceneButtons()
    {
        int maxSlots = 0;
        for (auto* col : gridContent.columns)
            maxSlots = juce::jmax (maxSlots, col->slots.size());

        // Add missing buttons
        while (sceneButtons.size() < maxSlots)
        {
            int s      = sceneButtons.size();
            auto* btn  = new SceneLaunchButton();
            btn->sceneIndex = s;
            btn->onLaunch   = [this, s] { if (onLaunchScene) onLaunchScene (s); };
            addAndMakeVisible (btn);
            sceneButtons.add (btn);
        }

        // Remove excess buttons (max shrunk after a delete)
        while (sceneButtons.size() > maxSlots)
            sceneButtons.removeLast (1);

        resized(); // re-positions all scene buttons
    }

    void setClipData    (int t, int s, const ClipData& d) { gridContent.setClipData    (t, s, d); }
    void setClipSelected(int t, int s)                    { gridContent.setClipSelected (t, s); }
    void setTrackSelected(int t)                          
    { 
        gridContent.setTrackSelected(t); 
        for (int i = 0; i < returnColumns.size(); ++i) {
            if (t == 1000 + i) {
                returnColumns[i]->isSelected = true;
            } else {
                returnColumns[i]->isSelected = false;
            }
            returnColumns[i]->repaint();
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

    void setReturnRmsLevel (int retIdx, float lvl)
    {
        if (juce::isPositiveAndBelow(retIdx, returnColumns.size())) {
            returnColumns[retIdx]->rmsLevel = lvl;
            returnColumns[retIdx]->repaintMixerStrip();
        }
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

        // Return columns (right-most, after master)
        for (int i = (int)returnColumns.size() - 1; i >= 0; --i) {
            returnColumns[i]->setBounds(b.removeFromRight(SV_RETURN_W));
        }

        // Group columns are now INLINE in gridContent (Ableton-style) - no right-panel space needed

        masterColumn->setBounds (masterB);

        for (int s = 0; s < sceneButtons.size(); ++s)
            sceneButtons[s]->setBounds (sceneStrip.getX(), SV_HEADER_H + s * SV_SLOT_H, SV_SCENE_W, SV_SLOT_H);

        gridViewport.setBounds (b);
        updateContentSize();
    }

    public:
    void updateContentSize()
    {
        auto vb = gridViewport.getBounds();
        if (vb.isEmpty()) return;

        int numCols  = gridContent.visibleColumnCount() + 1; // +1 always-visible drop zone
        int contentW = juce::jmax (vb.getWidth(), numCols * SV_TRACK_W);

        // Height is always pinned to the viewport height so each TrackColumn
        // has a fixed size.  The clip area scrolls internally via the
        // per-column slotsViewport; the mixer strip is always visible at the
        // bottom.  Growing contentH with slot count would defeat the viewport.
        bool needsHBar = contentW > vb.getWidth();
        int  barH      = needsHBar ? gridViewport.getScrollBarThickness() : 0;
        int  contentH  = vb.getHeight() - barH;

        gridContent.setSize (contentW, contentH);
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

    void mouseDown (const juce::MouseEvent& e) override
    {
        // Check if click was in the SCENE label area
        auto sceneLabelBounds = juce::Rectangle<int>(getWidth() - SV_SCENE_W, 0, SV_SCENE_W, SV_HEADER_H);
        if (sceneLabelBounds.contains(e.getPosition()))
        {
            if (onSceneLabelClicked)
                onSceneLabelClicked();
        }
    }

    // ── DragAndDropTarget (on SessionView so Viewport doesn't block delivery) ─
    bool isInterestedInDragSource (const SourceDetails& d) override
    {
        return d.description.toString().startsWith ("InstrumentDrag:") ||
               d.description.toString().startsWith ("EffectDrag:")     ||
               d.description.toString().startsWith ("PluginDrag:")     ||
               d.description.toString().startsWith ("ReturnRackDrag:");
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
        } else if (d.description.toString().startsWith ("PluginDrag:")) {
            // Pass the absolute plugin path — MainComponent detects the prefix.
            juce::String path = d.description.toString().fromFirstOccurrenceOf ("PluginDrag:", false, false);
            if (onInstrumentDropped) onInstrumentDropped (hit, "__PluginPath__:" + path);
        } else if (d.description.toString().startsWith ("ReturnRackDrag:")) {
            // Create a new return bus regardless of where on the view it was dropped.
            if (onReturnRackDropped) onReturnRackDropped();
        } else if (d.description.toString().startsWith ("EffectDrag:")) {
            // 1) Check group columns (2000+ indices) using content coords
            if (hit == -1) {
                for (int i = 0; i < gridContent.groupCols.size(); ++i) {
                    if (gridContent.groupCols[i]->getBounds().contains (contPos)) {
                        hit = 2000 + i;
                        break;
                    }
                }
            }
            // 2) Check return columns (1000+ indices)
            if (hit == -1) {
                for (int i = 0; i < returnColumns.size(); ++i) {
                    if (returnColumns[i]->getBounds().contains(d.localPosition)) {
                        hit = 1000 + i;
                        break;
                    }
                }
            }
            // 3) Fallback: currently selected track (Arrangement mode)
            if (hit == -1) {
                if (getSelectedTrackIndex) hit = getSelectedTrackIndex();
            }
            if (onEffectDropped && hit >= 0) onEffectDropped (hit, type);
        }
    }

private:
    juce::Viewport                      gridViewport;
    juce::OwnedArray<SceneLaunchButton> sceneButtons;
    std::vector<std::unique_ptr<FixedTrackColumn>> returnColumns;
    std::unique_ptr<FixedTrackColumn>   masterColumn;

public:
    // Exposed so MainComponent can update names after project load
    std::vector<std::unique_ptr<FixedTrackColumn>> groupColumns;

private:



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
