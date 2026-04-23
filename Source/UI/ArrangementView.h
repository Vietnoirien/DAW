#pragma once

#include <JuceHeader.h>
#include <functional>
#include "ClipData.h"

// ─── Layout Constants ─────────────────────────────────────────────────────────
static constexpr int ARR_HEADER_H = 36;
static constexpr int ARR_SLOT_H   = 36;
static constexpr int ARR_TRACK_W  = 180; // track header width (same as SessionView)

// ─── Arrangement-level automation overlay ─────────────────────────────────────
// Drawn as a transparent, fully-interactive component placed on top of a single
// track row inside ArrangementContent. It shows ALL automation points that belong
// to the currently-focused parameter across every arrangement clip on that track.
class ArrangementAutomationOverlay : public juce::Component
{
public:
    ArrangementAutomationOverlay() { setInterceptsMouseClicks(true, true); }

    // ── Data model ────────────────────────────────────────────────────────────
    // trackClips  : the ArrangementClip vector for the track being edited
    // paramId     : which parameter lane to display / edit
    // totalBars   : full visible arrangement length (sets the X scale)
    // pixelsPerBar: horizontal zoom level mirrored from ArrangementContent
    void setData(std::vector<ArrangementClip>* trackClips,
                 const juce::String& paramId,
                 double totalBars,
                 double pixelsPerBar)
    {
        clips        = trackClips;
        parameterId  = paramId;
        lenBars      = totalBars;
        ppb          = pixelsPerBar;
        repaint();
    }

    void clearData()
    {
        clips       = nullptr;
        parameterId = {};
        repaint();
    }

    // Call whenever the zoom or content width changes
    void updateScale(double totalBars, double pixelsPerBar)
    {
        lenBars = totalBars;
        ppb     = pixelsPerBar;
        repaint();
    }

    // ── Callbacks ─────────────────────────────────────────────────────────────
    std::function<void()> onAutomationChanged;

    // ── Paint ─────────────────────────────────────────────────────────────────
    void paint(juce::Graphics& g) override
    {
        if (clips == nullptr || parameterId.isEmpty()) return;

        // Semi-transparent dark tint over the track row so the curve stands out
        g.setColour(juce::Colour(0x55000000));
        g.fillRect(getLocalBounds());

        // Label
        g.setColour(juce::Colours::white.withAlpha(0.7f));
        g.setFont(juce::Font(juce::FontOptions(10.0f)));
        g.drawText("AUT: " + parameterId, 4, 2, getWidth() - 8, 14, juce::Justification::topLeft);

        // Draw grid lines (quarters)
        g.setColour(juce::Colour(0xff2a2a2a).withAlpha(0.5f));
        for (float frac = 0.25f; frac < 1.0f; frac += 0.25f)
            g.drawHorizontalLine(static_cast<int>(getHeight() * frac), 0.0f, static_cast<float>(getWidth()));

        // For each clip on this track that overlaps the visible area, draw its lane
        for (const auto& clip : *clips)
        {
            // Find the automation lane for the focused parameter
            const AutomationLane* lane = nullptr;
            for (const auto& l : clip.automationLanes)
                if (l.parameterId == parameterId) { lane = &l; break; }
            if (lane == nullptr || lane->points.empty()) continue;

            // Coordinate helpers
            double clipStartBars = clip.startBar - 1.0; // 0-based bars
            double clipLenBars   = clip.lengthBars;
            double clipLenBeats  = clipLenBars * 4.0;

            auto beatToX = [&](double beatInClip) -> float {
                double barInClip = beatInClip / 4.0;
                return static_cast<float>((clipStartBars + barInClip) * ppb);
            };

            auto valToY = [&](float v) -> float {
                return (1.0f - v) * static_cast<float>(getHeight());
            };

            // Draw filled area under the curve
            juce::Path filled;
            bool started = false;
            for (size_t i = 0; i < lane->points.size(); ++i) {
                float x = beatToX(lane->points[i].positionBeats);
                float y = valToY(lane->points[i].value);
                if (!started) { filled.startNewSubPath(x, static_cast<float>(getHeight())); filled.lineTo(x, y); started = true; }
                else filled.lineTo(x, y);
            }
            if (started) {
                filled.lineTo(beatToX(clipLenBeats), valToY(lane->points.back().value));
                filled.lineTo(beatToX(clipLenBeats), static_cast<float>(getHeight()));
                filled.closeSubPath();
            }
            g.setColour(juce::Colour(0xffff4444).withAlpha(0.18f));
            g.fillPath(filled);

            // Draw the curve line
            juce::Path path;
            for (size_t i = 0; i < lane->points.size(); ++i) {
                float x = beatToX(lane->points[i].positionBeats);
                float y = valToY(lane->points[i].value);
                if (i == 0) path.startNewSubPath(x, y); else path.lineTo(x, y);
            }
            g.setColour(juce::Colour(0xffff5555).withAlpha(0.9f));
            g.strokePath(path, juce::PathStrokeType(2.0f));

            // Draw control points
            for (size_t i = 0; i < lane->points.size(); ++i) {
                float x = beatToX(lane->points[i].positionBeats);
                float y = valToY(lane->points[i].value);
                bool dragged = (editingClipIndex >= 0
                                && editingClipIndex == clipIndexOf(clip)
                                && draggedPointIndex == (int)i);
                g.setColour(dragged ? juce::Colours::white : juce::Colour(0xffff8888));
                g.fillEllipse(x - 4.0f, y - 4.0f, 8.0f, 8.0f);
            }

            // Clip boundary lines
            g.setColour(juce::Colours::white.withAlpha(0.2f));
            g.drawVerticalLine(static_cast<int>(clipStartBars * ppb), 0.0f, static_cast<float>(getHeight()));
            g.drawVerticalLine(static_cast<int>((clipStartBars + clipLenBars) * ppb), 0.0f, static_cast<float>(getHeight()));
        }
    }

    // ── Mouse handling ────────────────────────────────────────────────────────
    void mouseDown(const juce::MouseEvent& e) override
    {
        if (clips == nullptr) return;
        editingClipIndex  = -1;
        draggedPointIndex = -1;

        for (int ci = 0; ci < (int)clips->size(); ++ci) {
            auto& clip = (*clips)[ci];
            AutomationLane* lane = findLane(clip);
            if (lane == nullptr) continue;

            for (int pi = 0; pi < (int)lane->points.size(); ++pi) {
                float x = beatToXGlobal(clip, lane->points[pi].positionBeats);
                float y = (1.0f - lane->points[pi].value) * getHeight();
                if (e.position.getDistanceFrom({x, y}) < 10.0f) {
                    editingClipIndex  = ci;
                    draggedPointIndex = pi;
                    break;
                }
            }
            if (editingClipIndex >= 0) break;
        }

        // Right-click: delete
        if (e.mods.isRightButtonDown() && editingClipIndex >= 0) {
            auto& lane = *findLane((*clips)[editingClipIndex]);
            bool isLeft  = (draggedPointIndex == 0 && lane.points[0].positionBeats == 0.0);
            bool isRight = (draggedPointIndex == (int)lane.points.size() - 1
                            && lane.points.back().positionBeats >= (*clips)[editingClipIndex].lengthBars * 4.0);
            if (!isLeft && !isRight) {
                lane.points.erase(lane.points.begin() + draggedPointIndex);
                if (onAutomationChanged) onAutomationChanged();
            }
            editingClipIndex  = -1;
            draggedPointIndex = -1;
            repaint();
            return;
        }

        // Double left-click on empty space: add point in the clip under the cursor
        if (e.mods.isLeftButtonDown() && e.getNumberOfClicks() == 2 && editingClipIndex < 0) {
            for (int ci = 0; ci < (int)clips->size(); ++ci) {
                auto& clip = (*clips)[ci];
                double clipStartX = (clip.startBar - 1.0) * ppb;
                double clipEndX   = clipStartX + clip.lengthBars * ppb;
                if (e.position.x >= clipStartX && e.position.x < clipEndX) {
                    AutomationLane* lane = findLane(clip);
                    if (lane == nullptr) {
                        clip.automationLanes.push_back({parameterId, {}});
                        lane = &clip.automationLanes.back();
                        double lb = clip.lengthBars * 4.0;
                        lane->points.push_back({0.0, 0.5f});
                        lane->points.push_back({lb,  0.5f});
                    }
                    double beatInClip = ((e.position.x - clipStartX) / ppb) * 4.0;
                    float  val        = 1.0f - juce::jlimit(0.0f, 1.0f, e.position.y / getHeight());
                    lane->points.push_back({beatInClip, val});
                    std::sort(lane->points.begin(), lane->points.end(),
                              [](const AutomationPoint& a, const AutomationPoint& b){ return a.positionBeats < b.positionBeats; });
                    if (onAutomationChanged) onAutomationChanged();
                    repaint();
                    break;
                }
            }
        }
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (clips == nullptr || editingClipIndex < 0 || draggedPointIndex < 0) return;
        auto& clip = (*clips)[editingClipIndex];
        AutomationLane* lane = findLane(clip);
        if (lane == nullptr) return;

        double clipStartX = (clip.startBar - 1.0) * ppb;
        double beatInClip = ((e.position.x - clipStartX) / ppb) * 4.0;
        float  val        = 1.0f - juce::jlimit(0.0f, 1.0f, e.position.y / getHeight());

        double clipLenBeats = clip.lengthBars * 4.0;
        bool isLeft  = (draggedPointIndex == 0 && lane->points[0].positionBeats == 0.0);
        bool isRight = (draggedPointIndex == (int)lane->points.size() - 1
                        && lane->points.back().positionBeats >= clipLenBeats);
        if      (isLeft)  beatInClip = 0.0;
        else if (isRight) beatInClip = clipLenBeats;
        else              beatInClip = juce::jlimit(0.0, clipLenBeats, beatInClip);

        AutomationPoint dragging = lane->points[draggedPointIndex];
        dragging.positionBeats = beatInClip;
        dragging.value         = val;
        lane->points[draggedPointIndex] = dragging;
        std::sort(lane->points.begin(), lane->points.end(),
                  [](const AutomationPoint& a, const AutomationPoint& b){ return a.positionBeats < b.positionBeats; });
        // Re-find the dragged point after sort
        for (int i = 0; i < (int)lane->points.size(); ++i) {
            if (lane->points[i].positionBeats == dragging.positionBeats
                && lane->points[i].value == dragging.value) {
                draggedPointIndex = i; break;
            }
        }
        if (onAutomationChanged) onAutomationChanged();
        repaint();
    }

    void mouseUp(const juce::MouseEvent&) override
    {
        draggedPointIndex = -1;
        editingClipIndex  = -1;
        repaint();
    }

private:
    std::vector<ArrangementClip>* clips = nullptr;
    juce::String   parameterId;
    double         lenBars = 16.0;
    double         ppb     = 80.0;  // pixels per bar

    int editingClipIndex  = -1;
    int draggedPointIndex = -1;

    AutomationLane* findLane(ArrangementClip& clip)
    {
        for (auto& l : clip.automationLanes)
            if (l.parameterId == parameterId) return &l;
        return nullptr;
    }

    float beatToXGlobal(const ArrangementClip& clip, double beatInClip) const
    {
        double barInClip = beatInClip / 4.0;
        return static_cast<float>((clip.startBar - 1.0 + barInClip) * ppb);
    }

    int clipIndexOf(const ArrangementClip& target) const
    {
        if (clips == nullptr) return -1;
        for (int i = 0; i < (int)clips->size(); ++i)
            if (&(*clips)[i] == &target) return i;
        return -1;
    }
};

class ArrangementView : public juce::Component
{
public:
    ArrangementView()
    {
        // Viewport – horizontal + vertical scroll
        gridViewport.setViewedComponent(&content, false);
        gridViewport.setScrollBarsShown(true, true);
        gridViewport.getHorizontalScrollBar().setColour(
            juce::ScrollBar::thumbColourId, juce::Colour(0xff334466));
        gridViewport.getVerticalScrollBar().setColour(
            juce::ScrollBar::thumbColourId, juce::Colour(0xff334466));
        addAndMakeVisible(gridViewport);

        // Automation overlay lives inside the content (scrolls with it)
        content.addChildComponent(automationOverlay);
    }

    void resized() override
    {
        auto b = getLocalBounds();
        gridViewport.setBounds(b);
        updateContentSize();
        positionAutomationOverlay();
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour(0xff0e0e1c));
    }

    // Callbacks
    std::function<void(int trackIdx, double startBar, const ClipData&)> onClipPlaced;
    std::function<void(ArrangementClip* clip)>                           onClipMoved;
    std::function<void(ArrangementClip* clip)>                           onClipDeleted;
    std::function<void(double barPosition)>                              onPlayheadScrubbed;
    std::function<void(double loopIn, double loopOut)>                   onLoopChanged;
    // Fired when the user clicks a track header or a track row to select it
    std::function<void(int trackIdx)>                                    onTrackSelected;
    // Fired when the user left-clicks a clip block to select it
    std::function<void(ArrangementClip* clip)>                           onClipSelected;

    // Currently selected clip (nullptr = none). Stored here so blocks can read it for drawing.
    ArrangementClip* selectedClip = nullptr;
    void setSelectedClip(ArrangementClip* c) { selectedClip = c; content.repaint(); }

    struct TrackState {
        juce::String name;
        juce::Colour colour {juce::Colour(0xff2d89ef)}; // Default color
        std::vector<ClipData> availableClips;
    };

    // Track which track is currently selected so the header can be highlighted
    int selectedTrack = -1;
    void setSelectedTrack(int t) { selectedTrack = t; content.repaint(); }

    void setPlayheadBar(double bar) {
        content.setPlayheadBar(bar);
    }

    void setTracksAndClips(const std::vector<TrackState>& tracks, const std::vector<ArrangementClip>* tracksClips) {
        content.setTracksAndClips(tracks, tracksClips);
        positionAutomationOverlay(); // reposition if track count changed
    }

    void repaintPlayhead() {
        content.repaint(); // Or specific area
    }

    // ── Arrangement Automation Overlay API ────────────────────────────────────
    // Show automation for 'paramId' on 'trackIndex'. trackClips must remain
    // valid for as long as the overlay is visible (owned by arrangementTracks).
    void setArrangementAutomation(int trackIndex,
                                  std::vector<ArrangementClip>* trackClips,
                                  const juce::String& paramId)
    {
        activeAutomationTrack = trackIndex;
        double totalBars = computeTotalBars();
        automationOverlay.setData(trackClips, paramId, totalBars, content.pixelsPerBar);
        automationOverlay.setVisible(true);
        positionAutomationOverlay();
    }

    void clearArrangementAutomation()
    {
        activeAutomationTrack = -1;
        automationOverlay.clearData();
        automationOverlay.setVisible(false);
    }

    // Forward the callback so MainComponent can re-sync the render engine
    std::function<void()> onArrangementAutomationChanged;

    ArrangementAutomationOverlay automationOverlay;

private:
    class ArrangementContent; // Forward declaration

    class ArrClipBlock : public juce::Component
    {
    public:
        ArrClipBlock(ArrangementClip& c, ArrangementContent& parent);

        void paint(juce::Graphics& g) override;
        void mouseDown(const juce::MouseEvent& e) override;
        void mouseDrag(const juce::MouseEvent& e) override;
        void mouseUp(const juce::MouseEvent& e) override;
        void mouseEnter(const juce::MouseEvent& e) override;
        void mouseExit(const juce::MouseEvent& e) override;
        void mouseMove(const juce::MouseEvent& e) override;

        ArrangementClip& clip;
        ArrangementContent& owner;

    private:
        bool isHovered = false;
        bool isSelected = false;
        
        enum class DragMode { None, Move, ResizeLeft, ResizeRight };
        DragMode dragMode = DragMode::None;
        
        double originalStartBar = 0;
        double originalLengthBars = 0;
        juce::Point<int> dragStartPos;

        DragMode getDragModeForPosition(int x) const;
    };
    class ArrangementContent : public juce::Component
    {
    public:
        void paint(juce::Graphics& g) override
        {
            g.fillAll(juce::Colour(0xff0e0e1c));

            // Draw Ruler background
            g.setColour(juce::Colour(0xff111120));
            g.fillRect(ARR_TRACK_W, 0, getWidth() - ARR_TRACK_W, ARR_HEADER_H);

            // Draw track header background
            g.setColour(juce::Colour(0xff161626));
            g.fillRect(0, 0, ARR_TRACK_W, getHeight());

            // Draw grid lines
            g.setColour(juce::Colour(0xff252540));
            g.drawHorizontalLine(ARR_HEADER_H, 0.0f, (float)getWidth());

            // Grab selected track index from parent for highlight
            int selTrack = -1;
            if (auto* parent = findParentComponentOfClass<ArrangementView>())
                selTrack = parent->selectedTrack;

            for (int t = 0; t < (int)trackStates.size(); ++t) {
                int y = ARR_HEADER_H + t * ARR_SLOT_H;

                // Selected track highlight on the full row
                if (t == selTrack) {
                    g.setColour(juce::Colour(0xff1e2a3a));
                    g.fillRect(0, y, getWidth(), ARR_SLOT_H);
                }

                g.setColour(juce::Colour(0xff2a2a40));
                g.drawHorizontalLine(y, 0.0f, (float)getWidth());

                // Track header badge (color)
                g.setColour(trackStates[t].colour);
                g.fillRect(0, y + 2, 4, ARR_SLOT_H - 4);

                // Selected track header background
                if (t == selTrack) {
                    g.setColour(juce::Colour(0xff223344));
                    g.fillRect(4, y + 2, ARR_TRACK_W - 6, ARR_SLOT_H - 4);
                }

                // Track header text
                g.setColour(t == selTrack ? juce::Colours::white : juce::Colours::lightgrey);
                g.setFont(12.0f);
                g.drawText(trackStates[t].name, 12, y, ARR_TRACK_W - 16, ARR_SLOT_H, juce::Justification::centredLeft);
            }

            // Draw vertical bar lines in ruler and timeline
            int numBars = std::max(16, (getWidth() - ARR_TRACK_W) / (int)pixelsPerBar);
            for (int b = 1; b <= numBars; ++b) {
                int x = barToPixel(b);
                if (x > getWidth()) break;
                
                // Ruler text
                g.setColour(juce::Colour(0xff667788));
                g.setFont(9.0f);
                g.drawText(juce::String(b), x + 2, 0, 40, ARR_HEADER_H, juce::Justification::bottomLeft);
                
                // Grid line
                g.setColour(juce::Colour(0xff222238));
                g.drawVerticalLine(x, ARR_HEADER_H, (float)getHeight());
            }

            // Draw playhead
            if (playheadBar >= 1.0) {
                int px = barToPixel(playheadBar);
                g.setColour(juce::Colour(0xff44ffaa));
                g.drawVerticalLine(px, 0.0f, (float)getHeight());
                
                // Playhead triangle in ruler
                juce::Path tri;
                tri.addTriangle(px - 5.0f, 0.0f, px + 5.0f, 0.0f, px, 8.0f);
                g.fillPath(tri);
            }
        }

        int barToPixel(double bar) const {
            return ARR_TRACK_W + static_cast<int>((bar - 1.0) * pixelsPerBar);
        }

        double pixelToBar(int x) const {
            return 1.0 + static_cast<double>(x - ARR_TRACK_W) / pixelsPerBar;
        }
        
        void mouseDown(const juce::MouseEvent& e) override {
            if (e.y < ARR_HEADER_H) return;

            int trackIdx = (e.y - ARR_HEADER_H) / ARR_SLOT_H;
            if (trackIdx < 0 || trackIdx >= (int)trackStates.size()) return;

            // ── Track header click: select track to show device view ───────────
            if (e.x < ARR_TRACK_W) {
                if (auto* parent = findParentComponentOfClass<ArrangementView>()) {
                    parent->selectedTrack = trackIdx;
                    if (parent->onTrackSelected) parent->onTrackSelected(trackIdx);
                }
                repaint();
                return;
            }

            // ── Timeline area ─────────────────────────────────────────────────
            // Also select the track on a plain left-click (so device view updates)
            if (e.mods.isLeftButtonDown() && !e.mods.isPopupMenu()) {
                if (auto* parent = findParentComponentOfClass<ArrangementView>()) {
                    if (parent->selectedTrack != trackIdx) {
                        parent->selectedTrack = trackIdx;
                        if (parent->onTrackSelected) parent->onTrackSelected(trackIdx);
                        repaint();
                    }
                }
            }

            if (e.mods.isPopupMenu()) {
                // If the automation overlay is active on this track, suppress the
                // clip-insert menu so the overlay's own right-click handler fires.
                if (auto* parent = findParentComponentOfClass<ArrangementView>()) {
                    if (parent->automationOverlay.isVisible()
                        && parent->activeAutomationTrack == trackIdx) {
                        // Let the event fall through to the overlay component
                        return;
                    }
                }

                // Right click empty area -> create clip
                double clickedBar = std::floor(pixelToBar(e.x));
                if (clickedBar < 1.0) clickedBar = 1.0;

                juce::PopupMenu m;
                const auto& trackClips = trackStates[trackIdx].availableClips;

                if (trackClips.empty()) {
                    m.addItem(1, "Create Empty Clip");
                } else {
                    m.addSectionHeader("Insert Clip");
                    for (size_t i = 0; i < trackClips.size(); ++i) {
                        m.addItem(static_cast<int>(i + 1), trackClips[i].name);
                    }
                }

                m.showMenuAsync(juce::PopupMenu::Options(), [this, trackIdx, clickedBar, trackClips](int result) {
                    if (result > 0) {
                        if (auto* parent = findParentComponentOfClass<ArrangementView>()) {
                            ClipData clipToPlace;
                            if (trackClips.empty()) {
                                clipToPlace.hasClip = true;
                                clipToPlace.name = "New Clip";
                                clipToPlace.patternLengthBars = 1.0;
                            } else {
                                clipToPlace = trackClips[static_cast<size_t>(result - 1)];
                            }
                            if (parent->onClipPlaced) parent->onClipPlaced(trackIdx, clickedBar, clipToPlace);
                        }
                    }
                });
            }
        }

        void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override {
            if (e.mods.isCommandDown() || e.mods.isCtrlDown()) {
                double zoomFactor = wheel.deltaY > 0 ? 1.2 : 0.8;
                
                // Keep the mouse position at the same bar
                double mouseBarBefore = pixelToBar(e.x);
                
                pixelsPerBar = juce::jlimit(10.0, 400.0, pixelsPerBar * zoomFactor);
                
                updateClipBounds();
                if (auto* parent = findParentComponentOfClass<ArrangementView>()) {
                    parent->updateContentSize();
                    // Adjust scroll to keep mouseBarBefore at e.x
                    int newMousePx = barToPixel(mouseBarBefore);
                    int currentScrollX = parent->gridViewport.getViewPositionX();
                    int newScrollX = currentScrollX + (newMousePx - e.x);
                    parent->gridViewport.setViewPosition(newScrollX, parent->gridViewport.getViewPositionY());
                }
                repaint();
            } else {
                // Let viewport handle normal scrolling
                Component::mouseWheelMove(e, wheel);
            }
        }

        void updateClipBounds() {
            for (auto* block : clipBlocks) {
                int x = barToPixel(block->clip.startBar);
                int w = static_cast<int>(block->clip.lengthBars * pixelsPerBar);
                int y = ARR_HEADER_H + block->clip.trackIndex * ARR_SLOT_H;
                block->setBounds(x, y + 2, w, ARR_SLOT_H - 4);
            }
            if (auto* parent = findParentComponentOfClass<ArrangementView>()) {
                parent->updateContentSize();
            }
        }

        void setTracksAndClips(const std::vector<TrackState>& tracks, const std::vector<ArrangementClip>* tracksClips) {
            trackStates = tracks;
            clipBlocks.clear();
            for (int t = 0; t < (int)trackStates.size(); ++t) {
                // Must cast away const to allow blocks to modify the clips via drag/resize
                auto& trackClips = const_cast<std::vector<ArrangementClip>&>(tracksClips[t]);
                for (auto& clip : trackClips) {
                    auto* block = new ArrClipBlock(clip, *this);
                    addAndMakeVisible(block);
                    clipBlocks.add(block);
                }
            }
            updateClipBounds();
            repaint();
            
            if (auto* parent = findParentComponentOfClass<ArrangementView>()) {
                parent->updateContentSize();
            }
        }

        void setPlayheadBar(double bar) {
            playheadBar = bar;
            repaint();
        }

        double playheadBar = 1.0;
        double pixelsPerBar = 80.0;
        std::vector<TrackState> trackStates;
        juce::OwnedArray<ArrClipBlock> clipBlocks;
    };

    juce::Viewport     gridViewport;
    ArrangementContent content;
    int activeAutomationTrack = -1;

    double computeTotalBars() const
    {
        double maxBars = 16.0;
        for (auto* block : content.clipBlocks) {
            double endBar = block->clip.startBar + block->clip.lengthBars;
            if (endBar > maxBars) maxBars = endBar;
        }
        return maxBars + 8.0;
    }

    // Place the overlay over the correct track row inside content
    void positionAutomationOverlay()
    {
        if (activeAutomationTrack < 0 || !automationOverlay.isVisible()) return;
        int y = ARR_HEADER_H + activeAutomationTrack * ARR_SLOT_H + 2;
        int h = ARR_SLOT_H - 4;
        automationOverlay.setBounds(ARR_TRACK_W, y, content.getWidth() - ARR_TRACK_W, h);
        automationOverlay.updateScale(computeTotalBars(), content.pixelsPerBar);
    }

    void updateContentSize()
    {
        auto vb = gridViewport.getBounds();
        if (vb.isEmpty()) return;

        double maxBars = computeTotalBars();

        int numTracks = content.trackStates.empty() ? 1 : (int)content.trackStates.size();
        int contentW = juce::jmax(vb.getWidth(), content.barToPixel(maxBars));
        int contentH = juce::jmax(vb.getHeight(), ARR_HEADER_H + numTracks * ARR_SLOT_H + 20);
        content.setSize(contentW, contentH);
        positionAutomationOverlay();
    }

    // ─── ArrClipBlock Implementation ──────────────────────────────────────────
    // Need to define it here since it uses ArrangementContent
    // (Moved outside the class definition below)
};

inline ArrangementView::ArrClipBlock::ArrClipBlock(ArrangementClip& c, ArrangementContent& parent)
    : clip(c), owner(parent)
{
    setRepaintsOnMouseActivity(true);
}

inline void ArrangementView::ArrClipBlock::paint(juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat();
    auto baseCol = clip.data.colour.darker(0.2f);
    
    if (isHovered) baseCol = baseCol.brighter(0.1f);
    
    g.setColour(baseCol);
    g.fillRoundedRectangle(b, 3.0f);
    
    if (isSelected) {
        g.setColour(juce::Colours::white);
        g.drawRoundedRectangle(b.reduced(1.0f), 3.0f, 1.5f);
    } else {
        g.setColour(clip.data.colour.darker(0.5f));
        g.drawRoundedRectangle(b, 3.0f, 1.0f);
    }
    
    // Draw name
    g.setColour(juce::Colours::white.withAlpha(0.9f));
    g.setFont(11.0f);
    g.drawText(clip.data.name, b.reduced(4, 0).toNearestInt(), juce::Justification::centredLeft);
    
    // Draw resize handles if hovered
    if (isHovered) {
        g.setColour(juce::Colours::white.withAlpha(0.5f));
        g.fillRect(0, 0, 4, getHeight());
        g.fillRect(getWidth() - 4, 0, 4, getHeight());
    }

    // Thick cyan outline = this clip is selected for automation
    if (auto* parent = owner.findParentComponentOfClass<ArrangementView>())
        if (parent->selectedClip == &clip)
        {
            g.setColour(juce::Colour(0xff00e5ff));
            g.drawRoundedRectangle(b.reduced(1.5f), 3.0f, 2.0f);
        }
}

inline ArrangementView::ArrClipBlock::DragMode ArrangementView::ArrClipBlock::getDragModeForPosition(int x) const
{
    if (x <= 5) return DragMode::ResizeLeft;
    if (x >= getWidth() - 5) return DragMode::ResizeRight;
    return DragMode::Move;
}

inline void ArrangementView::ArrClipBlock::mouseEnter(const juce::MouseEvent& e) { 
    isHovered = true; 
    setMouseCursor(getDragModeForPosition(e.x) == DragMode::Move ? juce::MouseCursor::NormalCursor : juce::MouseCursor::LeftRightResizeCursor);
    repaint(); 
}

inline void ArrangementView::ArrClipBlock::mouseExit(const juce::MouseEvent& e) { 
    isHovered = false; 
    setMouseCursor(juce::MouseCursor::NormalCursor);
    repaint(); 
}

inline void ArrangementView::ArrClipBlock::mouseMove(const juce::MouseEvent& e) {
    setMouseCursor(getDragModeForPosition(e.x) == DragMode::Move ? juce::MouseCursor::NormalCursor : juce::MouseCursor::LeftRightResizeCursor);
}

inline void ArrangementView::ArrClipBlock::mouseDown(const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu()) {
        juce::PopupMenu m;
        m.addItem(1, "Delete Clip");
        m.showMenuAsync(juce::PopupMenu::Options(), [this](int result) {
            if (result == 1) {
                if (auto* parent = owner.findParentComponentOfClass<ArrangementView>()) {
                    if (parent->onClipDeleted) parent->onClipDeleted(&clip);
                }
            }
        });
        return;
    }

    // Left-click: select this clip (track + clip) and start drag
    if (auto* parent = owner.findParentComponentOfClass<ArrangementView>()) {
        // Select the track if it changed
        if (parent->selectedTrack != clip.trackIndex) {
            parent->selectedTrack = clip.trackIndex;
            if (parent->onTrackSelected) parent->onTrackSelected(clip.trackIndex);
        }
        // Select the clip
        parent->selectedClip = &clip;
        if (parent->onClipSelected) parent->onClipSelected(&clip);
        owner.repaint();
    }

    dragMode = getDragModeForPosition(e.x);
    originalStartBar = clip.startBar;
    originalLengthBars = clip.lengthBars;
    dragStartPos = e.getEventRelativeTo(&owner).getPosition();
}

inline void ArrangementView::ArrClipBlock::mouseDrag(const juce::MouseEvent& e)
{
    if (dragMode == DragMode::None) return;
    
    auto currentPos = e.getEventRelativeTo(&owner).getPosition();
    double dxBars = (currentPos.x - dragStartPos.x) / owner.pixelsPerBar;
    
    // Snapping (snap to 1/4 bar by default, hold Shift for free)
    double snapInterval = e.mods.isShiftDown() ? 0.0 : 0.25;
    
    auto snap = [snapInterval](double val) {
        if (snapInterval > 0.0) return std::round(val / snapInterval) * snapInterval;
        return val;
    };
    
    if (dragMode == DragMode::Move) {
        double newStart = originalStartBar + dxBars;
        clip.startBar = std::max(1.0, snap(newStart));
        
        // Also update track index if dragged vertically
        int newTrackIdx = (currentPos.y - ARR_HEADER_H) / ARR_SLOT_H;
        newTrackIdx = juce::jlimit(0, (int)owner.trackStates.size() - 1, newTrackIdx);
        clip.trackIndex = newTrackIdx;
        
    } else if (dragMode == DragMode::ResizeRight) {
        double newLen = originalLengthBars + dxBars;
        clip.lengthBars = std::max(0.25, snap(newLen)); // min length 1/4 bar
    } else if (dragMode == DragMode::ResizeLeft) {
        double newStart = originalStartBar + dxBars;
        newStart = std::max(1.0, snap(newStart));
        double newLen = originalLengthBars - (newStart - originalStartBar);
        if (newLen >= 0.25) {
            clip.startBar = newStart;
            clip.lengthBars = newLen;
        }
    }
    
    owner.updateClipBounds();
}

inline void ArrangementView::ArrClipBlock::mouseUp(const juce::MouseEvent& e)
{
    if (dragMode != DragMode::None) {
        if (auto* parent = owner.findParentComponentOfClass<ArrangementView>()) {
            if (parent->onClipMoved) parent->onClipMoved(&clip);
        }
        dragMode = DragMode::None;
    }
}
