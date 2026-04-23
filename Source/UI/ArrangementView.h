#pragma once

#include <JuceHeader.h>
#include <functional>
#include "ClipData.h"

// ─── Layout Constants ─────────────────────────────────────────────────────────
static constexpr int ARR_HEADER_H = 36;
static constexpr int ARR_SLOT_H   = 36;
static constexpr int ARR_TRACK_W  = 180; // track header width (same as SessionView)

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
    }

    void resized() override
    {
        auto b = getLocalBounds();
        gridViewport.setBounds(b);
        updateContentSize();
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

    struct TrackState {
        juce::String name;
        juce::Colour colour {juce::Colour(0xff2d89ef)}; // Default color
        std::vector<ClipData> availableClips;
    };

    void setPlayheadBar(double bar) {
        content.setPlayheadBar(bar);
    }

    void setTracksAndClips(const std::vector<TrackState>& tracks, const std::vector<ArrangementClip>* tracksClips) {
        content.setTracksAndClips(tracks, tracksClips);
    }

    void repaintPlayhead() {
        content.repaint(); // Or specific area
    }

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
            
            for (int t = 0; t < (int)trackStates.size(); ++t) {
                int y = ARR_HEADER_H + t * ARR_SLOT_H;
                g.setColour(juce::Colour(0xff2a2a40));
                g.drawHorizontalLine(y, 0.0f, (float)getWidth());
                
                // Track header badge (color)
                g.setColour(trackStates[t].colour);
                g.fillRect(0, y + 2, 4, ARR_SLOT_H - 4);
                
                // Track header text
                g.setColour(juce::Colours::lightgrey);
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
            if (e.x < ARR_TRACK_W || e.y < ARR_HEADER_H) return;
            
            int trackIdx = (e.y - ARR_HEADER_H) / ARR_SLOT_H;
            if (trackIdx >= 0 && trackIdx < (int)trackStates.size()) {
                if (e.mods.isPopupMenu()) {
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

    void updateContentSize()
    {
        auto vb = gridViewport.getBounds();
        if (vb.isEmpty()) return;

        double maxBars = 16.0;
        for (auto* block : content.clipBlocks) {
            double endBar = block->clip.startBar + block->clip.lengthBars;
            if (endBar > maxBars) maxBars = endBar;
        }
        maxBars += 8.0; // 8 bars buffer

        int numTracks = content.trackStates.empty() ? 1 : (int)content.trackStates.size();
        int contentW = juce::jmax(vb.getWidth(), content.barToPixel(maxBars));
        int contentH = juce::jmax(vb.getHeight(), ARR_HEADER_H + numTracks * ARR_SLOT_H + 20);
        content.setSize(contentW, contentH);
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
