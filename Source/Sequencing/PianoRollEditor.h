#pragma once

#include <JuceHeader.h>
#include "ClipData.h"

// ─────────────────────────────────────────────────────────────────────────────
//  Bar / Beat ruler – drawn in a fixed header above the scrollable grid
// ─────────────────────────────────────────────────────────────────────────────
class PianoRollRuler : public juce::Component
{
public:
    PianoRollRuler() { setOpaque(true); }

    /** Call whenever the scroll x-offset or zoom changes. */
    void setScrollX(int x)
    {
        scrollX = x;
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        auto bounds = getLocalBounds();
        g.fillAll(juce::Colour(0xff16161e));

        // Bottom border
        g.setColour(juce::Colour(0xff3a3a55));
        g.drawLine(0, bounds.getBottom() - 1, bounds.getWidth(), bounds.getBottom() - 1, 1.0f);

        const int numBars   = 16;
        const int beatsPerBar = 4;
        const float totalBeats = numBars * beatsPerBar;
        const float pixelsPerBeat = beatWidth;

        for (int bar = 0; bar <= numBars; ++bar)
        {
            float x = bar * beatsPerBar * pixelsPerBeat - scrollX;

            // Bar line
            g.setColour(juce::Colour(0xff9090b0));
            g.drawLine(x, 0, x, (float)bounds.getHeight(), 1.5f);

            // Bar label
            if (x >= 0 && x < bounds.getWidth())
            {
                g.setColour(juce::Colour(0xffb0b0d0));
                g.setFont(juce::Font(juce::FontOptions(11.0f, juce::Font::bold)));
                g.drawText(juce::String(bar + 1),
                           (int)x + 3, 0, 40, bounds.getHeight() - 2,
                           juce::Justification::centredLeft);
            }

            // Beat ticks within bar
            for (int beat = 1; beat < beatsPerBar; ++beat)
            {
                float bx = x + beat * pixelsPerBeat;
                if (bx < 0 || bx > bounds.getWidth()) continue;
                g.setColour(juce::Colour(0xff504060));
                g.drawLine(bx, bounds.getHeight() * 0.5f, bx, (float)bounds.getHeight(), 1.0f);
            }
        }

        (void)totalBeats;
    }

    float beatWidth = 100.0f;

private:
    int scrollX = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
//  Piano Roll Editor – the note grid
// ─────────────────────────────────────────────────────────────────────────────
class PianoRollEditor : public juce::Component
{
public:
    PianoRollEditor()
    {
        setOpaque(true);
    }

    void setNotes(const std::vector<MidiNote>& newNotes)
    {
        notes = newNotes;
        repaint();
    }

    const std::vector<MidiNote>& getNotes() const { return notes; }

    // ── paint ──────────────────────────────────────────────────────────────
    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour(0xff1e1e28));

        auto bounds = getLocalBounds();

        // ── Horizontal rows (one per MIDI note) ──────────────────────────
        for (int i = 0; i < 128; ++i)
        {
            int y = i * noteHeight;
            int noteInOctave = (127 - i) % 12;
            bool isBlackKey  = (noteInOctave == 1 || noteInOctave == 3 ||
                                noteInOctave == 6 || noteInOctave == 8 ||
                                noteInOctave == 10);

            if (isBlackKey)
            {
                g.setColour(juce::Colour(0xff191924));
                g.fillRect(0, y, bounds.getWidth(), noteHeight);
            }

            g.setColour(juce::Colour(0xff28283a));
            g.drawLine(0, (float)(y + noteHeight), (float)bounds.getWidth(), (float)(y + noteHeight), 0.5f);
        }

        // ── Vertical grid lines ──────────────────────────────────────────
        const int numBars = 16;
        const float pixelsPerBeat = beatWidth;
        const float pixelsPer16th = pixelsPerBeat / 4.0f;
        const int total16ths = numBars * 16;

        for (int i = 0; i <= total16ths; ++i)
        {
            float x = i * pixelsPer16th;
            bool isBar   = (i % 16 == 0);
            bool isBeat  = (i %  4 == 0);

            if (isBar)
                g.setColour(juce::Colour(0xff5050aa));
            else if (isBeat)
                g.setColour(juce::Colour(0xff383860));
            else
                g.setColour(juce::Colour(0xff252538));

            g.drawLine(x, 0, x, (float)bounds.getHeight(), isBar ? 1.5f : 0.7f);
        }

        // ── Notes ────────────────────────────────────────────────────────
        for (int ni = 0; ni < (int)notes.size(); ++ni)
        {
            const auto& note = notes[ni];
            float x = (float)(note.startBeat  * beatWidth);
            float w = (float)(note.lengthBeats * beatWidth);
            float y = (127 - note.note) * (float)noteHeight;

            juce::Rectangle<float> noteRect(x, y + 1.0f, std::max(w - 1.0f, 2.0f), noteHeight - 2.0f);

            // Colour – highlight if being resized
            bool isResizing = (dragMode == DragMode::Resize && dragNoteIndex == ni);
            juce::Colour baseColour = isResizing ? juce::Colour(0xff50aaff)
                                                 : juce::Colour(0xff2d89ef);

            // Gradient fill
            juce::ColourGradient grad(baseColour.brighter(0.3f), noteRect.getX(), noteRect.getY(),
                                      baseColour.darker(0.2f), noteRect.getX(), noteRect.getBottom(),
                                      false);
            g.setGradientFill(grad);
            g.fillRoundedRectangle(noteRect, 3.0f);

            // Border
            g.setColour(juce::Colours::white.withAlpha(0.35f));
            g.drawRoundedRectangle(noteRect, 3.0f, 1.0f);

            // Resize handle – rightmost 6 px
            if (w > 10.0f)
            {
                juce::Rectangle<float> handle(noteRect.getRight() - 6.0f,
                                              noteRect.getY(),
                                              6.0f,
                                              noteRect.getHeight());
                g.setColour(juce::Colours::white.withAlpha(0.25f));
                g.fillRect(handle);
            }
        }
    }

    void resized() override {}

    // ── Mouse helpers ───────────────────────────────────────────────────────
    /** Returns the index of the note under pixel position, or -1. */
    int noteAtPosition(juce::Point<float> pos) const
    {
        for (int ni = (int)notes.size() - 1; ni >= 0; --ni)
        {
            const auto& n = notes[ni];
            float x = (float)(n.startBeat  * beatWidth);
            float w = (float)(n.lengthBeats * beatWidth);
            float y = (127 - n.note) * (float)noteHeight;

            juce::Rectangle<float> r(x, y, std::max(w, 2.0f), (float)noteHeight);
            if (r.contains(pos)) return ni;
        }
        return -1;
    }

    /** Returns true if pos is within the resize handle of note[ni]. */
    bool isOnResizeHandle(int ni, juce::Point<float> pos) const
    {
        const auto& n = notes[ni];
        float x = (float)(n.startBeat  * beatWidth);
        float w = (float)(n.lengthBeats * beatWidth);
        float y = (127 - n.note) * (float)noteHeight;
        juce::Rectangle<float> handle(x + w - 8.0f, y, 8.0f, (float)noteHeight);
        return handle.contains(pos);
    }

    float snapBeat(float rawBeat) const
    {
        return std::round(rawBeat * 4.0f) / 4.0f; // 16th-note grid
    }

    // ── Mouse events ────────────────────────────────────────────────────────
    void mouseDown(const juce::MouseEvent& e) override
    {
        dragMode = DragMode::None;
        dragNoteIndex = -1;

        int ni = noteAtPosition(e.position);

        if (e.mods.isRightButtonDown())
        {
            // Right-click → delete note
            if (ni >= 0)
            {
                notes.erase(notes.begin() + ni);
                if (onNotesChanged) onNotesChanged(notes);
                repaint();
            }
            return;
        }

        // Left-click
        if (ni >= 0)
        {
            if (isOnResizeHandle(ni, e.position))
            {
                // Start resize drag
                dragMode = DragMode::Resize;
                dragNoteIndex = ni;
                dragStartX = e.position.x;
                dragOriginalLength = (float)notes[ni].lengthBeats;
                lastAuditionedNote = notes[ni].note;
                if (onAuditionNoteOn) onAuditionNoteOn(lastAuditionedNote, (int)(notes[ni].velocity * 127.0f));
            }
            else
            {
                // Click on body → delete
                notes.erase(notes.begin() + ni);
                if (onNotesChanged) onNotesChanged(notes);
                repaint();
            }
        }
        else
        {
            // Empty space → place new note
            float beat    = e.position.x / beatWidth;
            int   noteNum = 127 - (int)(e.position.y / noteHeight);
            noteNum = juce::jlimit(0, 127, noteNum);
            float snapped = snapBeat(beat);

            MidiNote newNote;
            newNote.note        = noteNum;
            newNote.startBeat   = snapped;
            newNote.lengthBeats = 0.25;   // default: 1 sixteenth
            newNote.velocity    = 0.8f;

            notes.push_back(newNote);
            dragNoteIndex = (int)notes.size() - 1;
            dragMode      = DragMode::Resize;        // immediately allow dragging to extend
            dragStartX    = e.position.x;
            dragOriginalLength = (float)newNote.lengthBeats;

            lastAuditionedNote = newNote.note;
            if (onAuditionNoteOn) onAuditionNoteOn(lastAuditionedNote, (int)(newNote.velocity * 127.0f));

            if (onNotesChanged) onNotesChanged(notes);
            repaint();
        }
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (dragMode == DragMode::Resize &&
            dragNoteIndex >= 0 &&
            dragNoteIndex < (int)notes.size())
        {
            auto& note = notes[dragNoteIndex];
            float rawEnd    = e.position.x / beatWidth;
            float snappedEnd = snapBeat(rawEnd);
            float minLen    = 0.25f;
            float newLen    = snappedEnd - (float)note.startBeat;
            if (newLen < minLen) newLen = minLen;

            note.lengthBeats = newLen;
            if (onNotesChanged) onNotesChanged(notes);
            repaint();
        }
    }

    void mouseUp(const juce::MouseEvent&) override
    {
        dragMode      = DragMode::None;
        dragNoteIndex = -1;
        if (lastAuditionedNote != -1 && onAuditionNoteOff) {
            onAuditionNoteOff(lastAuditionedNote);
            lastAuditionedNote = -1;
        }
        repaint(); // remove resize highlight
    }

    void mouseMove(const juce::MouseEvent& e) override
    {
        // Update cursor: show resize cursor near right edge of notes
        int ni = noteAtPosition(e.position);
        if (ni >= 0 && isOnResizeHandle(ni, e.position))
            setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);
        else
            setMouseCursor(juce::MouseCursor::NormalCursor);
    }

    std::function<void(const std::vector<MidiNote>&)> onNotesChanged;
    std::function<void(int, int)> onAuditionNoteOn;
    std::function<void(int)> onAuditionNoteOff;

    float beatWidth  = 100.0f;
    int   noteHeight = 16;

private:
    enum class DragMode { None, Resize };

    std::vector<MidiNote> notes;
    DragMode dragMode      = DragMode::None;
    int      dragNoteIndex = -1;
    float    dragStartX    = 0.0f;
    float    dragOriginalLength = 0.0f;
    int      lastAuditionedNote = -1;
};

// ─────────────────────────────────────────────────────────────────────────────
//  Piano keyboard strip – left sidebar
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
            int octave       = (127 - i) / 12 - 2;
            bool isBlackKey  = (noteInOctave == 1 || noteInOctave == 3 ||
                                noteInOctave == 6 || noteInOctave == 8 ||
                                noteInOctave == 10);

            if (isBlackKey)
            {
                g.setColour(juce::Colour(0xff111118));
                g.fillRect(0, y, bounds.getWidth() * 2 / 3, noteHeight);
                g.setColour(juce::Colour(0xff2a2a38));
                g.fillRect(bounds.getWidth() * 2 / 3, y, bounds.getWidth() / 3, noteHeight);
            }
            else
            {
                g.setColour(juce::Colour(0xfff0f0f8));
                g.fillRect(0, y, bounds.getWidth(), noteHeight);
                g.setColour(juce::Colour(0xff888899));
                g.drawLine(0, (float)(y + noteHeight), (float)bounds.getWidth(), (float)(y + noteHeight), 0.5f);
            }

            // Note label on C
            if (noteInOctave == 0)
            {
                g.setColour(juce::Colour(0xff333355));
                g.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
                g.drawText("C" + juce::String(octave),
                           2, y, bounds.getWidth() - 4, noteHeight,
                           juce::Justification::centredLeft);
            }
        }

        // Right border
        g.setColour(juce::Colour(0xff3a3a55));
        g.drawLine((float)bounds.getWidth() - 1, 0,
                   (float)bounds.getWidth() - 1, (float)bounds.getHeight(), 1.5f);
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        int note = 127 - (int)(e.position.y / noteHeight);
        if (note >= 0 && note <= 127) {
            currentNote = note;
            if (onNoteOn) onNoteOn(note, 100);
        }
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        int note = 127 - (int)(e.position.y / noteHeight);
        if (note != currentNote) {
            if (currentNote != -1 && onNoteOff) onNoteOff(currentNote);
            if (note >= 0 && note <= 127) {
                currentNote = note;
                if (onNoteOn) onNoteOn(note, 100);
            } else {
                currentNote = -1;
            }
        }
    }

    void mouseUp(const juce::MouseEvent&) override
    {
        if (currentNote != -1 && onNoteOff) {
            onNoteOff(currentNote);
            currentNote = -1;
        }
    }

    std::function<void(int, int)> onNoteOn;
    std::function<void(int)> onNoteOff;

private:
    int noteHeight = 16;
    int currentNote = -1;
};

// ─────────────────────────────────────────────────────────────────────────────
//  PianoRollViewer – assembles ruler + keyboard + editor inside a Viewport
//  The keyboard is FIXED (outside the viewport) so it stays visible while
//  scrolling horizontally. Vertical scroll is propagated by listening to
//  the viewport's vertical scrollbar.
// ─────────────────────────────────────────────────────────────────────────────
class PianoRollViewer : public juce::Component,
                        private juce::ScrollBar::Listener
{
public:
    static constexpr int keyboardWidth = 60;
    static constexpr int rulerHeight   = 22;
    static constexpr int noteHeight    = 16;
    static constexpr int numNotes      = 128;
    static constexpr int numBars       = 16;
    static constexpr int beatsPerBar   = 4;
    static constexpr float beatWidth   = 100.0f;

    PianoRollViewer()
    {
        // Fixed overlay ruler (above the scroll area, right of keyboard)
        addAndMakeVisible(ruler);
        ruler.beatWidth = beatWidth;

        // Fixed keyboard – rendered with the current vertical scroll offset
        addAndMakeVisible(keyboardOverlay);

        // Scrollable viewport (only contains the editor grid – NO keyboard)
        addAndMakeVisible(viewport);
        viewport.setViewedComponent(&editor, false);
        viewport.setScrollBarsShown(true, true);

        editor.beatWidth  = beatWidth;
        editor.noteHeight = noteHeight;

        editor.onNotesChanged = [this](const std::vector<MidiNote>& notes) {
            if (onNotesChanged) onNotesChanged(notes);
        };

        editor.onAuditionNoteOn = [this](int note, int vel) {
            if (onAuditionNoteOn) onAuditionNoteOn(note, vel);
        };
        editor.onAuditionNoteOff = [this](int note) {
            if (onAuditionNoteOff) onAuditionNoteOff(note);
        };

        keyboardOverlay.onNoteOn = [this](int note, int vel) {
            if (onAuditionNoteOn) onAuditionNoteOn(note, vel);
        };
        keyboardOverlay.onNoteOff = [this](int note) {
            if (onAuditionNoteOff) onAuditionNoteOff(note);
        };

        // Sync ruler to horizontal scroll
        viewport.getHorizontalScrollBar().addListener(this);
        // Sync keyboard overlay to vertical scroll
        viewport.getVerticalScrollBar().addListener(this);
    }

    ~PianoRollViewer() override
    {
        viewport.getHorizontalScrollBar().removeListener(this);
        viewport.getVerticalScrollBar().removeListener(this);
    }

    void setNotes(const std::vector<MidiNote>& notes)
    {
        editor.setNotes(notes);
    }

    void resized() override
    {
        auto bounds = getLocalBounds();

        // ── Ruler: top strip, right of keyboard ──────────────────────────
        ruler.setBounds(keyboardWidth, 0, bounds.getWidth() - keyboardWidth, rulerHeight);

        // ── Fixed keyboard overlay: left column, below ruler ─────────────
        keyboardOverlay.setBounds(0, rulerHeight,
                                  keyboardWidth, bounds.getHeight() - rulerHeight);
        keyboardOverlay.setScrollOffset(currentScrollY);

        // ── Viewport: occupies the remaining area to the right ────────────
        viewport.setBounds(keyboardWidth, rulerHeight,
                           bounds.getWidth() - keyboardWidth,
                           bounds.getHeight() - rulerHeight);

        int totalHeight = numNotes  * noteHeight;
        int totalWidth  = (int)(numBars * beatsPerBar * beatWidth);

        editor.setBounds(0, 0, totalWidth, totalHeight);

        // Default scroll to C4 area
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

    // ── Keyboard overlay component ───────────────────────────────────────────
    // Paints piano keys directly, offset by scrollY. JUCE clips paint() to
    // the component's own bounds, so this never overdraws outside the left panel.
    struct KeyboardOverlay : public juce::Component
    {
        KeyboardOverlay() { setOpaque(true); }

        void setScrollOffset(int y)
        {
            if (scrollY != y) { scrollY = y; repaint(); }
        }

        void paint(juce::Graphics& g) override
        {
            const int w = getWidth();
            const int h = getHeight();
            g.fillAll(juce::Colour(0xff1a1a24));

            const int firstNote = scrollY / noteHeight;
            const int lastNote  = juce::jmin(127, (scrollY + h) / noteHeight + 1);

            for (int i = firstNote; i <= lastNote; ++i)
            {
                const int y            = i * noteHeight - scrollY;
                const int noteInOctave = (127 - i) % 12;
                const int octave       = (127 - i) / 12 - 2;
                const bool isBlack     = (noteInOctave == 1 || noteInOctave == 3 ||
                                          noteInOctave == 6 || noteInOctave == 8 ||
                                          noteInOctave == 10);

                if (isBlack)
                {
                    g.setColour(juce::Colour(0xff111118));
                    g.fillRect(0, y, w * 2 / 3, noteHeight);
                    g.setColour(juce::Colour(0xff2a2a38));
                    g.fillRect(w * 2 / 3, y, w / 3, noteHeight);
                }
                else
                {
                    g.setColour(juce::Colour(0xfff0f0f8));
                    g.fillRect(0, y, w, noteHeight);
                    g.setColour(juce::Colour(0xff888899));
                    g.drawLine(0.f, float(y + noteHeight), float(w), float(y + noteHeight), 0.5f);

                    if (noteInOctave == 0)
                    {
                        g.setColour(juce::Colour(0xff333355));
                        g.setFont(juce::Font(juce::FontOptions(10.f, juce::Font::bold)));
                        g.drawText("C" + juce::String(octave), 2, y, w - 4, noteHeight,
                                   juce::Justification::centredLeft);
                    }
                }
            }

            // Right border
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
            if (note != currentNote)
            {
                if (currentNote != -1 && onNoteOff) onNoteOff(currentNote);
                currentNote = note;
                if (onNoteOn) onNoteOn(currentNote, 100);
            }
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

    juce::Viewport       viewport;
    PianoRollEditor      editor;
    PianoRollRuler       ruler;
    KeyboardOverlay      keyboardOverlay;
    int                  currentScrollY = 0;
    bool                 hasScrolled    = false;
};
