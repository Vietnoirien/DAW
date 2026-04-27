#pragma once

#include <JuceHeader.h>
#include "ClipData.h"
#include <set>
#include <map>

// ─────────────────────────────────────────────────────────────────────────────
//  Bar / Beat ruler
// ─────────────────────────────────────────────────────────────────────────────
class PianoRollRuler : public juce::Component
{
public:
    PianoRollRuler() { setOpaque(true); }

    void setScrollX(int x) { scrollX = x; repaint(); }

    void paint(juce::Graphics& g) override
    {
        auto bounds = getLocalBounds();
        g.fillAll(juce::Colour(0xff16161e));

        g.setColour(juce::Colour(0xff3a3a55));
        g.drawLine(0, bounds.getBottom() - 1, bounds.getWidth(), bounds.getBottom() - 1, 1.0f);

        const int numBars     = 16;
        const int beatsPerBar = 4;
        const float ppb       = beatWidth;

        for (int bar = 0; bar <= numBars; ++bar)
        {
            float x = bar * beatsPerBar * ppb - scrollX;
            g.setColour(juce::Colour(0xff9090b0));
            g.drawLine(x, 0, x, (float)bounds.getHeight(), 1.5f);
            if (x >= 0 && x < bounds.getWidth())
            {
                g.setColour(juce::Colour(0xffb0b0d0));
                g.setFont(juce::Font(juce::FontOptions(11.0f, juce::Font::bold)));
                g.drawText(juce::String(bar + 1), (int)x + 3, 0, 40, bounds.getHeight() - 2, juce::Justification::centredLeft);
            }
            for (int beat = 1; beat < beatsPerBar; ++beat)
            {
                float bx = x + beat * ppb;
                if (bx < 0 || bx > bounds.getWidth()) continue;
                g.setColour(juce::Colour(0xff504060));
                g.drawLine(bx, bounds.getHeight() * 0.5f, bx, (float)bounds.getHeight(), 1.0f);
            }
        }
    }

    float beatWidth = 100.0f;

private:
    int scrollX = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
//  Piano Roll Editor – note grid with selection, move, rect-select, dbl-click delete
// ─────────────────────────────────────────────────────────────────────────────
class PianoRollEditor : public juce::Component
{
public:
    PianoRollEditor() { setOpaque(true); }

    void setNotes(const std::vector<MidiNote>& newNotes)
    {
        notes = newNotes;
        selectedIndices.clear();
        repaint();
    }

    const std::vector<MidiNote>& getNotes() const { return notes; }

    // ── paint ──────────────────────────────────────────────────────────────
    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour(0xff1e1e28));
        auto bounds = getLocalBounds();

        // Horizontal rows
        for (int i = 0; i < 128; ++i)
        {
            int y = i * noteHeight;
            int noteInOctave = (127 - i) % 12;
            bool isBlack = (noteInOctave==1||noteInOctave==3||noteInOctave==6||noteInOctave==8||noteInOctave==10);
            if (isBlack) { g.setColour(juce::Colour(0xff191924)); g.fillRect(0, y, bounds.getWidth(), noteHeight); }
            g.setColour(juce::Colour(0xff28283a));
            g.drawLine(0, (float)(y + noteHeight), (float)bounds.getWidth(), (float)(y + noteHeight), 0.5f);
        }

        // Vertical grid
        const int numBars = 16;
        const float ppb = beatWidth;
        const float pp16 = ppb / 4.0f;
        const int total16 = numBars * 16;
        for (int i = 0; i <= total16; ++i)
        {
            float x = i * pp16;
            bool isBar = (i % 16 == 0), isBeat = (i % 4 == 0);
            g.setColour(isBar ? juce::Colour(0xff5050aa) : (isBeat ? juce::Colour(0xff383860) : juce::Colour(0xff252538)));
            g.drawLine(x, 0, x, (float)bounds.getHeight(), isBar ? 1.5f : 0.7f);
        }

        // Notes
        for (int ni = 0; ni < (int)notes.size(); ++ni)
        {
            const auto& note = notes[ni];
            float x = (float)(note.startBeat * beatWidth);
            float w = (float)(note.lengthBeats * beatWidth);
            float y = (127 - note.note) * (float)noteHeight;
            juce::Rectangle<float> nr(x, y + 1.0f, std::max(w - 1.0f, 2.0f), noteHeight - 2.0f);

            bool isSelected = (selectedIndices.count(ni) > 0);
            bool isResizing = (dragMode == DragMode::Resize && dragNoteIndex == ni);

            // ── 4.1 MPE: colour note by pressure (blue = soft → red = hard) ──
            // Only when the note has MPE snapshot data; otherwise default blue.
            juce::Colour mpeBase = juce::Colour(0xff2d89ef); // default blue
            if (note.hasMpe)
            {
                float p = juce::jlimit(0.0f, 1.0f, note.pressure);
                mpeBase = juce::Colour(0xff2d89ef).interpolatedWith(juce::Colour(0xffef2d2d), p);
            }

            juce::Colour base = isSelected  ? juce::Colour(0xffff9d00)
                              : isResizing  ? juce::Colour(0xff50aaff)
                                            : mpeBase;

            juce::ColourGradient grad(base.brighter(0.3f), nr.getX(), nr.getY(),
                                      base.darker(0.2f), nr.getX(), nr.getBottom(), false);
            g.setGradientFill(grad);
            g.fillRoundedRectangle(nr, 3.0f);

            g.setColour(isSelected ? juce::Colours::orange.brighter(0.5f)
                                   : juce::Colours::white.withAlpha(0.35f));
            g.drawRoundedRectangle(nr, 3.0f, isSelected ? 1.5f : 1.0f);

            if (w > 10.0f)
            {
                juce::Rectangle<float> handle(nr.getRight() - 6.0f, nr.getY(), 6.0f, nr.getHeight());
                g.setColour(juce::Colours::white.withAlpha(0.25f));
                g.fillRect(handle);
            }

            // ── 4.1 MPE: pitch-bend indicator beneath note block ─────────────
            // A short yellow line + dot showing the direction of the stored bend.
            if (note.hasMpe && std::abs(note.pitchBend) > 0.05f)
            {
                const float bendY   = nr.getBottom() + 2.0f;
                const float centreX = nr.getCentreX();
                // Scale: full note width = full ±48-semitone range
                const float bendDx  = (note.pitchBend / 48.0f) * (nr.getWidth() * 0.5f);
                const float endX    = centreX + bendDx;
                g.setColour(juce::Colours::yellow.withAlpha(0.75f));
                g.drawLine(centreX, bendY, endX, bendY, 1.5f);
                g.fillEllipse(endX - 2.0f, bendY - 2.0f, 4.0f, 4.0f);
            }
        }

        // Selection rectangle
        if (dragMode == DragMode::RectSelect)
        {
            auto sr = getSelectionRect();
            g.setColour(juce::Colour(0x44aaddff));
            g.fillRect(sr);
            g.setColour(juce::Colour(0xffaaddff));
            g.drawRect(sr, 1.0f);
        }
    }

    void resized() override {}

    // ── Helpers ─────────────────────────────────────────────────────────────
    int noteAtPosition(juce::Point<float> pos) const
    {
        for (int ni = (int)notes.size() - 1; ni >= 0; --ni)
        {
            const auto& n = notes[ni];
            float x = (float)(n.startBeat * beatWidth);
            float w = (float)(n.lengthBeats * beatWidth);
            float y = (127 - n.note) * (float)noteHeight;
            juce::Rectangle<float> r(x, y, std::max(w, 2.0f), (float)noteHeight);
            if (r.contains(pos)) return ni;
        }
        return -1;
    }

    bool isOnResizeHandle(int ni, juce::Point<float> pos) const
    {
        const auto& n = notes[ni];
        float x = (float)(n.startBeat * beatWidth);
        float w = (float)(n.lengthBeats * beatWidth);
        float y = (127 - n.note) * (float)noteHeight;
        juce::Rectangle<float> handle(x + w - 8.0f, y, 8.0f, (float)noteHeight);
        return handle.contains(pos);
    }

    float snapBeat(float raw) const { return std::round(raw * 4.0f) / 4.0f; }

    juce::Rectangle<float> getSelectionRect() const
    {
        return juce::Rectangle<float>::leftTopRightBottom(
            std::min(rectSelectStart.x, rectSelectCurrent.x),
            std::min(rectSelectStart.y, rectSelectCurrent.y),
            std::max(rectSelectStart.x, rectSelectCurrent.x),
            std::max(rectSelectStart.y, rectSelectCurrent.y));
    }

    void selectNotesInRect(juce::Rectangle<float> r, bool addToSelection)
    {
        if (!addToSelection) selectedIndices.clear();
        for (int ni = 0; ni < (int)notes.size(); ++ni)
        {
            const auto& n = notes[ni];
            float x = (float)(n.startBeat * beatWidth);
            float w = (float)(n.lengthBeats * beatWidth);
            float y = (127 - n.note) * (float)noteHeight;
            juce::Rectangle<float> nr(x, y, std::max(w, 2.0f), (float)noteHeight);
            if (r.intersects(nr)) selectedIndices.insert(ni);
        }
    }

    // ── Mouse events ─────────────────────────────────────────────────────────
    void mouseDown(const juce::MouseEvent& e) override
    {
        dragMode = DragMode::None;
        dragNoteIndex = -1;

        int ni = noteAtPosition(e.position);

        if (e.mods.isRightButtonDown())
        {
            if (ni >= 0) { notes.erase(notes.begin() + ni); selectedIndices.clear(); if (onNotesChanged) onNotesChanged(notes); repaint(); }
            return;
        }

        // Double-click → delete note
        if (e.getNumberOfClicks() == 2 && ni >= 0)
        {
            notes.erase(notes.begin() + ni);
            selectedIndices.clear();
            if (onNotesChanged) onNotesChanged(notes);
            repaint();
            return;
        }

        if (ni >= 0)
        {
            if (isOnResizeHandle(ni, e.position))
            {
                // Start resize
                dragMode = DragMode::Resize;
                dragNoteIndex = ni;
                dragStartX = e.position.x;
                dragOriginalLength = (float)notes[ni].lengthBeats;
                lastAuditionedNote = notes[ni].note;
                if (onAuditionNoteOn) onAuditionNoteOn(lastAuditionedNote, (int)(notes[ni].velocity * 127.0f));
            }
            else
            {
                // Select note
                bool addToSel = e.mods.isShiftDown() || e.mods.isCommandDown();
                if (!addToSel && selectedIndices.count(ni) == 0)
                    selectedIndices.clear();
                if (addToSel && selectedIndices.count(ni) > 0)
                    selectedIndices.erase(ni);
                else
                    selectedIndices.insert(ni);

                // Prepare move drag
                dragMode = DragMode::Move;
                dragNoteIndex = ni;
                dragStartX = e.position.x;
                dragStartY = e.position.y;
                // Save original positions for all selected
                dragOriginalNotes.clear();
                for (int idx : selectedIndices) dragOriginalNotes[idx] = notes[idx];
                lastAuditionedNote = notes[ni].note;
                if (onAuditionNoteOn) onAuditionNoteOn(lastAuditionedNote, (int)(notes[ni].velocity * 127.0f));
                repaint();
            }
        }
        else
        {
            // Empty space
            bool addToSel = e.mods.isShiftDown() || e.mods.isCommandDown();
            if (!addToSel) selectedIndices.clear();

            // Start rectangle selection
            rectSelectStart   = e.position;
            rectSelectCurrent = e.position;
            dragMode = DragMode::RectSelect;
            repaint();
        }
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (dragMode == DragMode::Resize && dragNoteIndex >= 0 && dragNoteIndex < (int)notes.size())
        {
            auto& note = notes[dragNoteIndex];
            float rawEnd = e.position.x / beatWidth;
            float snapped = snapBeat(rawEnd);
            float newLen = snapped - (float)note.startBeat;
            if (newLen < 0.25f) newLen = 0.25f;
            note.lengthBeats = newLen;
            if (onNotesChanged) onNotesChanged(notes);
            repaint();
        }
        else if (dragMode == DragMode::Move && dragNoteIndex >= 0 && !dragOriginalNotes.empty())
        {
            float dx = e.position.x - dragStartX;
            float dy = e.position.y - dragStartY;
            float dBeats = snapBeat(dx / beatWidth);
            int   dNotes = (int)std::round(dy / noteHeight);

            for (auto& [idx, orig] : dragOriginalNotes)
            {
                auto& n = notes[idx];
                float newStart = (float)orig.startBeat + dBeats;
                if (newStart < 0.0f) newStart = 0.0f;
                n.startBeat = newStart;
                int newNote = orig.note - dNotes;
                n.note = juce::jlimit(0, 127, newNote);
            }
            if (onNotesChanged) onNotesChanged(notes);
            repaint();
        }
        else if (dragMode == DragMode::RectSelect)
        {
            rectSelectCurrent = e.position;
            bool addToSel = e.mods.isShiftDown() || e.mods.isCommandDown();
            selectNotesInRect(getSelectionRect(), addToSel);
            repaint();
        }
    }

    void mouseUp(const juce::MouseEvent& e) override
    {
        if (dragMode == DragMode::RectSelect)
        {
            auto sr = getSelectionRect();
            // Tiny rect = simple click on empty space → place a new note
            if (sr.getWidth() < 4.0f && sr.getHeight() < 4.0f)
            {
                placeNote(rectSelectStart);
                return;
            }
            selectNotesInRect(sr, e.mods.isShiftDown() || e.mods.isCommandDown());
        }

        dragMode = DragMode::None;
        dragNoteIndex = -1;
        dragOriginalNotes.clear();

        if (lastAuditionedNote != -1 && onAuditionNoteOff)
        {
            onAuditionNoteOff(lastAuditionedNote);
            lastAuditionedNote = -1;
        }
        repaint();
    }

    void mouseMove(const juce::MouseEvent& e) override
    {
        int ni = noteAtPosition(e.position);
        if (ni >= 0 && isOnResizeHandle(ni, e.position))
            setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);
        else if (ni >= 0)
            setMouseCursor(juce::MouseCursor::DraggingHandCursor);
        else
            setMouseCursor(juce::MouseCursor::NormalCursor);
    }

    // Place a new note (called from empty-space single click if no rect select)
    void placeNote(juce::Point<float> pos)
    {
        float beat = pos.x / beatWidth;
        int noteNum = 127 - (int)(pos.y / noteHeight);
        noteNum = juce::jlimit(0, 127, noteNum);
        float snapped = snapBeat(beat);

        MidiNote newNote;
        newNote.note        = noteNum;
        newNote.startBeat   = snapped;
        newNote.lengthBeats = 0.25;
        newNote.velocity    = 0.8f;

        notes.push_back(newNote);
        selectedIndices.clear();
        selectedIndices.insert((int)notes.size() - 1);

        dragNoteIndex = (int)notes.size() - 1;
        dragMode = DragMode::Resize;
        dragStartX = pos.x;
        dragOriginalLength = (float)newNote.lengthBeats;

        lastAuditionedNote = newNote.note;
        if (onAuditionNoteOn) onAuditionNoteOn(lastAuditionedNote, (int)(newNote.velocity * 127.0f));
        if (onNotesChanged) onNotesChanged(notes);
        repaint();
    }

    std::function<void(const std::vector<MidiNote>&)> onNotesChanged;
    std::function<void(int, int)> onAuditionNoteOn;
    std::function<void(int)>      onAuditionNoteOff;

    float beatWidth  = 100.0f;
    int   noteHeight = 16;

private:
    enum class DragMode { None, Resize, Move, RectSelect };

    std::vector<MidiNote>        notes;
    std::set<int>                selectedIndices;
    DragMode                     dragMode         = DragMode::None;
    int                          dragNoteIndex    = -1;
    float                        dragStartX       = 0.0f;
    float                        dragStartY       = 0.0f;
    float                        dragOriginalLength = 0.0f;
    std::map<int, MidiNote>      dragOriginalNotes;
    juce::Point<float>           rectSelectStart;
    juce::Point<float>           rectSelectCurrent;
    int                          lastAuditionedNote = -1;
};

// ─────────────────────────────────────────────────────────────────────────────
//  Piano keyboard strip
// ─────────────────────────────────────────────────────────────────────────────
class PianoRollKeyboard : public juce::Component
{
public:
    PianoRollKeyboard() { setOpaque(true); }

    void paint(juce::Graphics& g) override
    {
        auto bounds = getLocalBounds();
        g.fillAll(juce::Colour(0xff1a1a24));

        for (int i = 0; i < 128; ++i)
        {
            int y = i * noteHeight;
            int noteInOctave = (127 - i) % 12;
            int octave = (127 - i) / 12 - 2;
            bool isBlack = (noteInOctave==1||noteInOctave==3||noteInOctave==6||noteInOctave==8||noteInOctave==10);

            if (isBlack)
            {
                g.setColour(juce::Colour(0xff111118)); g.fillRect(0, y, bounds.getWidth() * 2 / 3, noteHeight);
                g.setColour(juce::Colour(0xff2a2a38)); g.fillRect(bounds.getWidth() * 2 / 3, y, bounds.getWidth() / 3, noteHeight);
            }
            else
            {
                g.setColour(juce::Colour(0xfff0f0f8)); g.fillRect(0, y, bounds.getWidth(), noteHeight);
                g.setColour(juce::Colour(0xff888899)); g.drawLine(0, (float)(y + noteHeight), (float)bounds.getWidth(), (float)(y + noteHeight), 0.5f);
            }
            if (noteInOctave == 0)
            {
                g.setColour(juce::Colour(0xff333355));
                g.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
                g.drawText("C" + juce::String(octave), 2, y, bounds.getWidth() - 4, noteHeight, juce::Justification::centredLeft);
            }
        }
        g.setColour(juce::Colour(0xff3a3a55));
        g.drawLine((float)bounds.getWidth() - 1, 0, (float)bounds.getWidth() - 1, (float)bounds.getHeight(), 1.5f);
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        currentNote = 127 - (int)(e.position.y / noteHeight);
        if (currentNote >= 0 && currentNote <= 127 && onNoteOn) onNoteOn(currentNote, 100);
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        int note = 127 - (int)(e.position.y / noteHeight);
        if (note != currentNote)
        {
            if (currentNote != -1 && onNoteOff) onNoteOff(currentNote);
            currentNote = note;
            if (note >= 0 && note <= 127 && onNoteOn) onNoteOn(note, 100);
            else currentNote = -1;
        }
    }

    void mouseUp(const juce::MouseEvent&) override
    {
        if (currentNote != -1 && onNoteOff) { onNoteOff(currentNote); currentNote = -1; }
    }

    std::function<void(int, int)> onNoteOn;
    std::function<void(int)>      onNoteOff;

private:
    int noteHeight  = 16;
    int currentNote = -1;
};

// ─────────────────────────────────────────────────────────────────────────────
//  PianoRollViewer
// ─────────────────────────────────────────────────────────────────────────────
class PianoRollViewer : public juce::Component,
                        private juce::ScrollBar::Listener
{
public:
    static constexpr int   keyboardWidth = 60;
    static constexpr int   rulerHeight   = 22;
    static constexpr int   noteHeight    = 16;
    static constexpr int   numNotes      = 128;
    static constexpr int   numBars       = 16;
    static constexpr int   beatsPerBar   = 4;
    static constexpr float beatWidth     = 100.0f;

    PianoRollViewer()
    {
        addAndMakeVisible(ruler);
        ruler.beatWidth = beatWidth;

        addAndMakeVisible(keyboardOverlay);

        addAndMakeVisible(viewport);
        viewport.setViewedComponent(&editor, false);
        viewport.setScrollBarsShown(true, true);

        editor.beatWidth  = beatWidth;
        editor.noteHeight = noteHeight;

        editor.onNotesChanged = [this](const std::vector<MidiNote>& n) { if (onNotesChanged) onNotesChanged(n); };
        editor.onAuditionNoteOn  = [this](int n, int v) { if (onAuditionNoteOn)  onAuditionNoteOn(n, v); };
        editor.onAuditionNoteOff = [this](int n)        { if (onAuditionNoteOff) onAuditionNoteOff(n); };

        keyboardOverlay.onNoteOn  = [this](int n, int v) { if (onAuditionNoteOn)  onAuditionNoteOn(n, v); };
        keyboardOverlay.onNoteOff = [this](int n)        { if (onAuditionNoteOff) onAuditionNoteOff(n); };

        viewport.getHorizontalScrollBar().addListener(this);
        viewport.getVerticalScrollBar().addListener(this);
    }

    ~PianoRollViewer() override
    {
        viewport.getHorizontalScrollBar().removeListener(this);
        viewport.getVerticalScrollBar().removeListener(this);
    }

    void setNotes(const std::vector<MidiNote>& notes) { editor.setNotes(notes); }

    void resized() override
    {
        auto b = getLocalBounds();
        ruler.setBounds(keyboardWidth, 0, b.getWidth() - keyboardWidth, rulerHeight);
        keyboardOverlay.setBounds(0, rulerHeight, keyboardWidth, b.getHeight() - rulerHeight);
        keyboardOverlay.setScrollOffset(currentScrollY);
        viewport.setBounds(keyboardWidth, rulerHeight, b.getWidth() - keyboardWidth, b.getHeight() - rulerHeight);

        editor.setBounds(0, 0, (int)(numBars * beatsPerBar * beatWidth), numNotes * noteHeight);

        if (!hasScrolled)
        {
            int c4Y = (127 - 60) * noteHeight;
            int startY = juce::jmax(0, c4Y - viewport.getHeight() / 2);
            viewport.setViewPosition(0, startY);
            currentScrollY = startY;
            keyboardOverlay.setScrollOffset(currentScrollY);
            hasScrolled = true;
        }
    }

    std::function<void(const std::vector<MidiNote>&)> onNotesChanged;
    std::function<void(int, int)> onAuditionNoteOn;
    std::function<void(int)>      onAuditionNoteOff;

private:
    void scrollBarMoved(juce::ScrollBar* sb, double) override
    {
        if (sb == &viewport.getHorizontalScrollBar())
            ruler.setScrollX((int)sb->getCurrentRangeStart());
        if (sb == &viewport.getVerticalScrollBar())
        {
            currentScrollY = (int)sb->getCurrentRangeStart();
            keyboardOverlay.setScrollOffset(currentScrollY);
        }
    }

    // ── Keyboard overlay ─────────────────────────────────────────────────────
    struct KeyboardOverlay : public juce::Component
    {
        KeyboardOverlay() { setOpaque(true); }

        void setScrollOffset(int y) { if (scrollY != y) { scrollY = y; repaint(); } }

        void paint(juce::Graphics& g) override
        {
            const int w = getWidth(), h = getHeight();
            g.fillAll(juce::Colour(0xff1a1a24));
            const int first = scrollY / noteHeight;
            const int last  = juce::jmin(127, (scrollY + h) / noteHeight + 1);
            for (int i = first; i <= last; ++i)
            {
                const int y = i * noteHeight - scrollY;
                const int nio = (127 - i) % 12;
                const int oct = (127 - i) / 12 - 2;
                const bool isBlack = (nio==1||nio==3||nio==6||nio==8||nio==10);
                if (isBlack)
                {
                    g.setColour(juce::Colour(0xff111118)); g.fillRect(0, y, w * 2 / 3, noteHeight);
                    g.setColour(juce::Colour(0xff2a2a38)); g.fillRect(w * 2 / 3, y, w / 3, noteHeight);
                }
                else
                {
                    g.setColour(juce::Colour(0xfff0f0f8)); g.fillRect(0, y, w, noteHeight);
                    g.setColour(juce::Colour(0xff888899)); g.drawLine(0.f, float(y + noteHeight), float(w), float(y + noteHeight), 0.5f);
                    if (nio == 0) { g.setColour(juce::Colour(0xff333355)); g.setFont(juce::Font(juce::FontOptions(10.f, juce::Font::bold))); g.drawText("C" + juce::String(oct), 2, y, w - 4, noteHeight, juce::Justification::centredLeft); }
                }
            }
            g.setColour(juce::Colour(0xff3a3a55));
            g.drawLine(float(w) - 1.f, 0.f, float(w) - 1.f, float(h), 1.5f);
        }

        void mouseDown(const juce::MouseEvent& e) override
        {
            currentNote = juce::jlimit(0, 127, 127 - (((int)e.position.y + scrollY) / noteHeight));
            if (onNoteOn) onNoteOn(currentNote, 100);
        }

        void mouseDrag(const juce::MouseEvent& e) override
        {
            int note = juce::jlimit(0, 127, 127 - (((int)e.position.y + scrollY) / noteHeight));
            if (note != currentNote) { if (currentNote != -1 && onNoteOff) onNoteOff(currentNote); currentNote = note; if (onNoteOn) onNoteOn(currentNote, 100); }
        }

        void mouseUp(const juce::MouseEvent&) override
        {
            if (currentNote != -1 && onNoteOff) { onNoteOff(currentNote); currentNote = -1; }
        }

        std::function<void(int, int)> onNoteOn;
        std::function<void(int)>      onNoteOff;

    private:
        int scrollY     = 0;
        int currentNote = -1;
        static constexpr int noteHeight = 16;
    };

    juce::Viewport   viewport;
    PianoRollEditor  editor;
    PianoRollRuler   ruler;
    KeyboardOverlay  keyboardOverlay;
    int              currentScrollY = 0;
    bool             hasScrolled    = false;
};
