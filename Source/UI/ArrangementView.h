#pragma once

#include <JuceHeader.h>
#include <functional>
#include "ClipData.h"

// ─── Layout Constants ─────────────────────────────────────────────────────────
static constexpr int ARR_HEADER_H = 36;
static constexpr int ARR_SLOT_H   = 36;
static constexpr int ARR_TRACK_W  = 180; // track header width (same as SessionView)
static constexpr int ARR_AUTO_H   = 80;  // height of automation lane when expanded

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

        // Background: dark navy tint so the lane is clearly distinct from the track
        g.setColour(juce::Colour(0xff0a0a1a));
        g.fillRect(getLocalBounds());

        // Top border to visually separate from the track row above
        g.setColour(juce::Colour(0xff00e5ff).withAlpha(0.6f));
        g.drawHorizontalLine(0, 0.0f, static_cast<float>(getWidth()));

        // Bottom border
        g.setColour(juce::Colour(0xff334455));
        g.drawHorizontalLine(getHeight() - 1, 0.0f, static_cast<float>(getWidth()));

        // Horizontal grid lines (value guides at 25%, 50%, 75%)
        g.setColour(juce::Colour(0xff1e2a3a));
        for (float frac = 0.25f; frac < 1.0f; frac += 0.25f)
            g.drawHorizontalLine(static_cast<int>(getHeight() * frac), 0.0f, static_cast<float>(getWidth()));

        // Label (left edge, inside the lane)
        g.setColour(juce::Colour(0xff00e5ff).withAlpha(0.85f));
        g.setFont(juce::Font(juce::FontOptions(10.0f)).boldened());
        g.drawText(juce::String::fromUTF8("\u2B83 ") + parameterId,
                   4, 2, 200, 14, juce::Justification::topLeft);
        g.setColour(juce::Colours::white.withAlpha(0.35f));
        g.setFont(juce::Font(juce::FontOptions(9.0f)));
        g.drawText("dbl-click: add  |  drag: move  |  right-click: delete",
                   4, getHeight() - 14, getWidth() - 8, 12, juce::Justification::bottomLeft);

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

            // Clip background tint so the lane region is easy to spot
            float cx0 = static_cast<float>(clipStartBars * ppb);
            float cx1 = static_cast<float>((clipStartBars + clipLenBars) * ppb);
            g.setColour(juce::Colour(0xff1a2030));
            g.fillRect(cx0, 0.0f, cx1 - cx0, static_cast<float>(getHeight()));

            auto beatToX = [&](double beatInClip) -> float {
                double barInClip = beatInClip / 4.0;
                return static_cast<float>((clipStartBars + barInClip) * ppb);
            };

            auto valToY = [&](float v) -> float {
                // Reserve 2px top/bottom padding so the curve is never clipped
                const float pad = 2.0f;
                return pad + (1.0f - v) * (static_cast<float>(getHeight()) - 2.0f * pad);
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
            g.setColour(juce::Colour(0xffff6633).withAlpha(0.28f));
            g.fillPath(filled);

            // Draw the curve line — bright and thick
            juce::Path path;
            for (size_t i = 0; i < lane->points.size(); ++i) {
                float x = beatToX(lane->points[i].positionBeats);
                float y = valToY(lane->points[i].value);
                if (i == 0) path.startNewSubPath(x, y); else path.lineTo(x, y);
            }
            g.setColour(juce::Colour(0xffff7744));
            g.strokePath(path, juce::PathStrokeType(2.5f));

            // Draw control points (larger for easier clicking)
            for (size_t i = 0; i < lane->points.size(); ++i) {
                float x = beatToX(lane->points[i].positionBeats);
                float y = valToY(lane->points[i].value);
                bool dragged = (editingClipIndex >= 0
                                && editingClipIndex == clipIndexOf(clip)
                                && draggedPointIndex == (int)i);
                // Outer glow ring
                g.setColour(juce::Colour(0xffff7744).withAlpha(0.4f));
                g.fillEllipse(x - 7.0f, y - 7.0f, 14.0f, 14.0f);
                // Inner dot
                g.setColour(dragged ? juce::Colours::white : juce::Colour(0xffffaa88));
                g.fillEllipse(x - 5.0f, y - 5.0f, 10.0f, 10.0f);
            }

            // Clip boundary lines
            g.setColour(juce::Colour(0xff00e5ff).withAlpha(0.35f));
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

// ─────────────────────────────────────────────────────────────────────────────
// TakeLaneOverlay (Phase 3.2)
// Drawn as a transparent, fully-interactive component placed beneath a single
// track row inside ArrangementContent (mirroring ArrangementAutomationOverlay).
// Displays N take rows (one row per take) with swipe-to-comp gesture support.
// ─────────────────────────────────────────────────────────────────────────────
static constexpr int ARR_TAKE_LANE_H = 22; // height per take row in pixels

class TakeLaneOverlay : public juce::Component
{
public:
    TakeLaneOverlay()
    {
        setInterceptsMouseClicks(true, true);
        // Thumbnail cache shared across all take lanes (4 MB)
        thumbCache = std::make_unique<juce::AudioThumbnailCache>(128);
    }

    // ── Data model ────────────────────────────────────────────────────────────
    // trackClips: the ArrangementClip vector for the track being displayed.
    // clipIdx:    which clip in the vector to show takes for.
    // totalBars / pixelsPerBar: layout scale matching ArrangementContent.
    void setData(std::vector<ArrangementClip>* trackClips,
                 int                           clipIdx,
                 double                        totalBars,
                 double                        pixelsPerBar)
    {
        clips   = trackClips;
        clipIdx_ = clipIdx;
        lenBars = totalBars;
        ppb     = pixelsPerBar;
        rebuildThumbnails();
        repaint();
    }

    void clearData()
    {
        clips   = nullptr;
        clipIdx_ = -1;
        thumbnails.clear();
        repaint();
    }

    void updateScale(double totalBars, double pixelsPerBar)
    {
        lenBars = totalBars;
        ppb     = pixelsPerBar;
        repaint();
    }

    // Returns the total height this overlay needs (numTakes × ARR_TAKE_LANE_H).
    int requiredHeight() const
    {
        if (clips == nullptr || clipIdx_ < 0 || clipIdx_ >= (int)clips->size()) return 0;
        return (int)(*clips)[clipIdx_].takes.size() * ARR_TAKE_LANE_H;
    }

    // ── Callbacks ─────────────────────────────────────────────────────────────
    // Called on mouseUp after swipe; wired to MainComponent::onCompRegionSwiped.
    std::function<void(int /*clipIdx*/, int /*takeIdx*/, double /*startBeat*/, double /*endBeat*/)> onCompRegionSwiped;

    // ── Paint ─────────────────────────────────────────────────────────────────
    void paint(juce::Graphics& g) override
    {
        if (clips == nullptr || clipIdx_ < 0 || clipIdx_ >= (int)clips->size()) return;
        const auto& clip = (*clips)[clipIdx_];
        if (clip.takes.empty()) return;

        const int numTakes = (int)clip.takes.size();
        const float w = static_cast<float>(getWidth());

        for (int ti = 0; ti < numTakes; ++ti)
        {
            const juce::Rectangle<float> rowR(0.0f, static_cast<float>(ti * ARR_TAKE_LANE_H),
                                              w, static_cast<float>(ARR_TAKE_LANE_H));

            // Row background
            g.setColour(juce::Colour(0xff0a0f18));
            g.fillRect(rowR);

            // Take label strip (left 18 px)
            g.setColour(juce::Colour(0xff1a2233));
            g.fillRect(rowR.withWidth(18.0f));
            g.setColour(juce::Colour(0xff8899aa));
            g.setFont(juce::Font(9.0f, juce::Font::bold));
            g.drawText("T" + juce::String(ti + 1), 1, ti * ARR_TAKE_LANE_H, 16, ARR_TAKE_LANE_H,
                       juce::Justification::centred, false);

            // Determine active regions for this take
            for (const auto& cr : clip.compRegions)
            {
                if (cr.takeIndex != ti) continue;
                float rx = beatToX(cr.startBeat);
                float rw = beatToX(cr.endBeat) - rx;
                if (rw <= 0.0f) continue;

                // Active region fill (teal)
                juce::Rectangle<float> crRect(rx, static_cast<float>(ti * ARR_TAKE_LANE_H) + 1.0f,
                                              rw, static_cast<float>(ARR_TAKE_LANE_H) - 2.0f);
                g.setColour(juce::Colour(0xff00c8a0).withAlpha(0.75f));
                g.fillRect(crRect);
                g.setColour(juce::Colour(0xff00e5c0));
                g.drawRect(crRect, 1.0f);

                // Waveform thumbnail (if loaded)
                if (ti < (int)thumbnails.size() && thumbnails[ti] != nullptr)
                {
                    g.setColour(juce::Colours::white.withAlpha(0.55f));
                    thumbnails[ti]->drawChannels(g, crRect.toNearestInt(), 0.0, thumbnails[ti]->getTotalLength(), 1.0f);
                }
            }

            // Draw muted regions (greyed out) — regions on OTHER takes that overlap this row's beat range
            // (simple visual: just draw the row background dimly where no active region exists)
            // Row separator line
            g.setColour(juce::Colour(0xff1c2a3c));
            g.drawHorizontalLine((ti + 1) * ARR_TAKE_LANE_H - 1, 18.0f, w);
        }

        // Swipe preview overlay
        if (isSwiping_)
        {
            float sx = beatToX(std::min(swipeStartBeat_, swipeEndBeat_));
            float sw = beatToX(std::max(swipeStartBeat_, swipeEndBeat_)) - sx;
            float sy = static_cast<float>(swipeTake_ * ARR_TAKE_LANE_H) + 1.0f;
            g.setColour(juce::Colour(0xffffffff).withAlpha(0.18f));
            g.fillRect(sx, sy, sw, static_cast<float>(ARR_TAKE_LANE_H) - 2.0f);
        }
    }

    // ── Mouse ─────────────────────────────────────────────────────────────────
    void mouseDown(const juce::MouseEvent& e) override
    {
        swipeStartBeat_ = xToBeat(static_cast<float>(e.x));
        swipeEndBeat_   = swipeStartBeat_;
        swipeTake_      = juce::jlimit(0, 99, e.y / ARR_TAKE_LANE_H);
        isSwiping_      = true;
        repaint();
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (!isSwiping_) return;
        swipeEndBeat_ = xToBeat(static_cast<float>(e.x));
        repaint();
    }

    void mouseUp(const juce::MouseEvent& e) override
    {
        if (!isSwiping_) return;
        isSwiping_ = false;
        double startB = std::min(swipeStartBeat_, swipeEndBeat_);
        double endB   = std::max(swipeStartBeat_, swipeEndBeat_);
        if (endB - startB > 0.01 && onCompRegionSwiped)
            onCompRegionSwiped(clipIdx_, swipeTake_, startB, endB);
        repaint();
    }

private:
    std::vector<ArrangementClip>* clips   = nullptr;
    int                           clipIdx_ = -1;
    double                        lenBars = 32.0;
    double                        ppb     = 40.0;

    // Swipe gesture state
    bool   isSwiping_     = false;
    int    swipeTake_     = 0;
    double swipeStartBeat_ = 0.0;
    double swipeEndBeat_   = 0.0;

    std::unique_ptr<juce::AudioThumbnailCache>          thumbCache;
    juce::OwnedArray<juce::AudioThumbnail>              thumbnails;
    juce::AudioFormatManager                            fmtMgr;

    float beatToX(double beat) const noexcept
    {
        return static_cast<float>(beat * ppb + 18.0); // 18 = label strip width
    }

    double xToBeat(float x) const noexcept
    {
        return std::max(0.0, (static_cast<double>(x) - 18.0) / ppb);
    }

    void rebuildThumbnails()
    {
        thumbnails.clear();
        if (clips == nullptr || clipIdx_ < 0 || clipIdx_ >= (int)clips->size()) return;
        fmtMgr.registerBasicFormats();
        const auto& clip = (*clips)[clipIdx_];
        for (const auto& take : clip.takes)
        {
            auto* thumb = new juce::AudioThumbnail(512, fmtMgr, *thumbCache);
            juce::File f(take.audioFilePath);
            if (f.existsAsFile()) thumb->setSource(new juce::FileInputSource(f));
            thumbnails.add(thumb);
        }
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TakeLaneOverlay)
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

        // Automation overlay lives inside the content (scrolls with it).
        // Added AFTER clip blocks are created, so it is raised to the top automatically.
        content.addChildComponent(automationOverlay);
        // Ensure the overlay is always painted on top of clip blocks
        automationOverlay.setAlwaysOnTop(true);

        // Take Lane overlay (3.2) — lives at the same level as the automation overlay.
        content.addChildComponent(takeLaneOverlay);
        takeLaneOverlay.setAlwaysOnTop(true);
        takeLaneOverlay.onCompRegionSwiped = [this](int clipIdx, int takeIdx,
                                                    double startBeat, double endBeat)
        {
            if (onCompRegionSwiped)
                onCompRegionSwiped(activeTakeLaneTrack, clipIdx, takeIdx, startBeat, endBeat);
        };
    }

    void resized() override
    {
        auto b = getLocalBounds();
        gridViewport.setBounds(b);
        updateContentSize();
        positionAutomationOverlay();
        positionTakeLaneOverlay();
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
    std::function<void(int trackIndex, bool armed)>                      onTrackArmChanged;
    std::function<void(int trackIndex, const juce::String& instrumentType)> onInstrumentDropped;
    std::function<void(int trackIndex, const juce::String& effectType)>     onEffectDropped;

    // Currently selected clip (nullptr = none). Stored here so blocks can read it for drawing.
    ArrangementClip* selectedClip = nullptr;
    void setSelectedClip(ArrangementClip* c) { selectedClip = c; content.repaint(); }

    struct TrackState {
        juce::String name;
        juce::Colour colour {juce::Colour(0xff2d89ef)}; // Default color
        bool isArmed {false};
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
        positionAutomationOverlay();  // reposition if track count changed
        positionTakeLaneOverlay();
    }

    void repaintPlayhead() {
        content.repaint(); // Or specific area
    }

    // ── Loop region API ───────────────────────────────────────────────────────
    // Called by MainComponent to sync loop state (e.g. after project load or
    // after the transport engine processed a loop wrap).
    void setLoopRegion(double inBar, double outBar, bool active) {
        content.loopInBar   = inBar;
        content.loopOutBar  = outBar;
        content.loopActive  = active;
        content.repaint();
    }

    void clearLoopRegion() {
        content.loopActive  = false;
        content.loopInBar   = 1.0;
        content.loopOutBar  = 1.0;
        content.repaint();
    }

    bool getLoopActive()   const { return content.loopActive; }
    double getLoopInBar()  const { return content.loopInBar; }
    double getLoopOutBar() const { return content.loopOutBar; }

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

    // ── Take Lane Overlay API (3.2) ───────────────────────────────────────────
    // Show take lanes for the clip at clipIdx on trackIndex.
    // trackClips must remain valid for as long as the overlay is visible.
    void setTakeLane(int trackIndex, int clipIdx, std::vector<ArrangementClip>* trackClips)
    {
        activeTakeLaneTrack = trackIndex;
        double totalBars = computeTotalBars();
        takeLaneOverlay.setData(trackClips, clipIdx, totalBars, content.pixelsPerBar);
        takeLaneOverlay.setVisible(true);
        updateContentSize();
        positionTakeLaneOverlay();
    }

    void clearTakeLane()
    {
        activeTakeLaneTrack = -1;
        takeLaneOverlay.clearData();
        takeLaneOverlay.setVisible(false);
        updateContentSize();
    }

    // Fired by TakeLaneOverlay on swipe completion.
    // Signature: (trackIdx, clipIdx, takeIdx, startBeat, endBeat)
    std::function<void(int, int, int, double, double)> onCompRegionSwiped;

    // (3.2) Fired when the user clicks "Show Take Lanes" on a clip block.
    // Signature: (trackIdx, ArrangementClip* clip)
    // MainComponent handles this by calling setTakeLane with the correct trackClips ptr.
    std::function<void(int, ArrangementClip*)> onShowTakeLanes;

    ArrangementAutomationOverlay automationOverlay;
    TakeLaneOverlay              takeLaneOverlay;

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
    class ArrangementContent : public juce::Component,
                               public juce::DragAndDropTarget
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

            // Find which track has its automation lane open (if any)
            int autoTrack = -1;
            if (auto* parent = findParentComponentOfClass<ArrangementView>())
                autoTrack = parent->activeAutomationTrack;

            // Running Y offset: tracks shift down when a lane is inserted above them
            int runningY = ARR_HEADER_H;
            for (int t = 0; t < (int)trackStates.size(); ++t) {
                int y = runningY;

                // Selected track highlight on the full row
                if (t == selTrack) {
                    g.setColour(juce::Colour(0xff1e2a3a));
                    g.fillRect(0, y, getWidth(), ARR_SLOT_H);
                }

                g.setColour(juce::Colour(0xff2a2a40));
                g.drawHorizontalLine(y, 0.0f, (float)getWidth());

                // Highlight track if dragging over it
                if (dragOverTrackIndex == t) {
                    g.setColour(juce::Colour(0x261a7a4a));
                    g.fillRect(0, y, getWidth(), ARR_SLOT_H);
                    
                    // Dashed border around the track
                    juce::Path solid;
                    solid.addRectangle(ARR_TRACK_W + 5.0f, y + 2.0f, getWidth() - ARR_TRACK_W - 10.0f, ARR_SLOT_H - 4.0f);
                    juce::Path dashed;
                    float dash[] = { 5.0f, 4.0f };
                    juce::PathStrokeType(1.3f).createDashedStroke(dashed, solid, dash, 2);
                    g.setColour(juce::Colour(0xff44aa77));
                    g.strokePath(dashed, juce::PathStrokeType(1.3f));
                }

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
                g.drawText(trackStates[t].name, 12, y, ARR_TRACK_W - 40, ARR_SLOT_H, juce::Justification::centredLeft);

                // Record Arm indicator
                juce::Rectangle<int> armRect(ARR_TRACK_W - 24, y + (ARR_SLOT_H - 16) / 2, 16, 16);
                g.setColour(trackStates[t].isArmed ? juce::Colour(0xffff3333) : juce::Colour(0xff333333));
                g.fillRoundedRectangle(armRect.toFloat(), 3.0f);
                g.setColour(trackStates[t].isArmed ? juce::Colours::white : juce::Colours::grey);
                g.setFont(juce::Font(juce::FontOptions(10.0f)).boldened());
                g.drawText(juce::String::fromUTF8("●"), armRect, juce::Justification::centred);

                runningY += ARR_SLOT_H;

                // If this track has its automation lane open, paint a header label in the
                // left-hand track header area so the lane is clearly identified
                if (t == autoTrack) {
                    g.setColour(juce::Colour(0xff0d1520));
                    g.fillRect(0, runningY, ARR_TRACK_W, ARR_AUTO_H);
                    g.setColour(juce::Colour(0xff00e5ff).withAlpha(0.6f));
                    g.drawHorizontalLine(runningY, 0.0f, static_cast<float>(ARR_TRACK_W));
                    g.setColour(trackStates[t].colour);
                    g.fillRect(0, runningY + 2, 4, ARR_AUTO_H - 4);
                    g.setColour(juce::Colour(0xff00e5ff));
                    g.setFont(juce::Font(juce::FontOptions(10.0f)).boldened());
                    g.drawText("AUTO", 10, runningY, ARR_TRACK_W - 12, ARR_AUTO_H,
                               juce::Justification::centredLeft);
                    runningY += ARR_AUTO_H;
                }
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

            // Draw loop region bracket in ruler
            if (loopActive && loopInBar < loopOutBar) {
                int lx0 = barToPixel(loopInBar);
                int lx1 = barToPixel(loopOutBar);
                // Filled tint
                g.setColour(juce::Colour(0xffffa500).withAlpha(0.22f));
                g.fillRect(lx0, 0, lx1 - lx0, ARR_HEADER_H);
                // Extend tint into track area
                g.setColour(juce::Colour(0xffffa500).withAlpha(0.06f));
                g.fillRect(lx0, ARR_HEADER_H, lx1 - lx0, getHeight() - ARR_HEADER_H);
                // In/Out marker lines
                g.setColour(juce::Colour(0xffffa500).withAlpha(0.9f));
                g.drawVerticalLine(lx0, 0.0f, (float)ARR_HEADER_H);
                g.drawVerticalLine(lx1, 0.0f, (float)ARR_HEADER_H);
                // Bracket caps
                g.fillRect(lx0, 0, 2, ARR_HEADER_H);
                g.fillRect(lx1 - 1, 0, 2, ARR_HEADER_H);
                // Label
                g.setColour(juce::Colour(0xffffa500));
                g.setFont(juce::Font(juce::FontOptions(9.0f)).boldened());
                int labelW = lx1 - lx0 - 4;
                if (labelW > 18)
                    g.drawText("LOOP", lx0 + 4, 2, labelW, 12, juce::Justification::centredLeft);
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
        
        // Helper: resolve the clicked Y coordinate to a track index, accounting
        // for the ARR_AUTO_H gap that the automation lane inserts below its track.
        // Returns -1 if the click lands in the ruler, the automation lane itself,
        // or below all tracks.
        int yToTrackIndex(int clickY) const
        {
            if (clickY < ARR_HEADER_H) return -1;

            int autoTrack = -1;
            if (auto* parent = findParentComponentOfClass<ArrangementView>())
                if (parent->automationOverlay.isVisible())
                    autoTrack = parent->activeAutomationTrack;

            int runningY = ARR_HEADER_H;
            for (int t = 0; t < (int)trackStates.size(); ++t) {
                // Is the click inside this track row?
                if (clickY >= runningY && clickY < runningY + ARR_SLOT_H)
                    return t;
                runningY += ARR_SLOT_H;

                // Skip over the automation lane if it belongs to this track
                if (t == autoTrack) {
                    if (clickY >= runningY && clickY < runningY + ARR_AUTO_H)
                        return -2; // inside the automation lane — handled by the overlay
                    runningY += ARR_AUTO_H;
                }
            }
            return -1; // below all tracks
        }

        void mouseDown(const juce::MouseEvent& e) override {
            // ── Ruler click / drag ──────────────────────────────────────────────
            if (e.y < ARR_HEADER_H && e.x > ARR_TRACK_W) {
                if (e.mods.isRightButtonDown()) {
                    // Right-click: context menu to manage loop
                    juce::PopupMenu m;
                    m.addItem(1, loopActive ? "Disable Loop" : "Enable Loop");
                    m.addItem(2, "Clear Loop");
                    m.showMenuAsync(juce::PopupMenu::Options(), [this](int result) {
                        if (result == 1) {
                            loopActive = !loopActive;
                            if (auto* parent = findParentComponentOfClass<ArrangementView>())
                                if (parent->onLoopChanged)
                                    parent->onLoopChanged(loopActive ? loopInBar : -1.0,
                                                          loopActive ? loopOutBar : -1.0);
                            repaint();
                        } else if (result == 2) {
                            loopActive = false;
                            loopInBar  = 1.0;
                            loopOutBar = 1.0;
                            if (auto* parent = findParentComponentOfClass<ArrangementView>())
                                if (parent->onLoopChanged) parent->onLoopChanged(-1.0, -1.0);
                            repaint();
                        }
                    });
                    return;
                }
                // Left-click or left-drag start
                double clickedBar = pixelToBar(e.x);
                clickedBar = std::max(1.0, clickedBar);
                rulerDragStart = clickedBar;
                // Snap to nearest bar for cleaner UX
                // (hold Shift for sub-bar precision)
                if (!e.mods.isShiftDown())
                    clickedBar = std::round(clickedBar);
                rulerDragInProgress = true;
                // Fire seek immediately so clicking alone also repositions playhead
                if (auto* parent = findParentComponentOfClass<ArrangementView>())
                    if (parent->onPlayheadScrubbed) parent->onPlayheadScrubbed(clickedBar);
                return;
            }

            int trackIdx = yToTrackIndex(e.y);
            // -1 = ruler / below tracks, -2 = inside automation lane (overlay handles it)
            if (trackIdx < 0) return;
            if (trackIdx >= (int)trackStates.size()) return;

            // ── Track header click: select track or arm ───────────
            if (e.x < ARR_TRACK_W) {
                juce::Rectangle<int> armRect(ARR_TRACK_W - 24, trackIdx * ARR_SLOT_H + ARR_HEADER_H + (ARR_SLOT_H - 16) / 2, 16, 16);
                if (armRect.contains(e.x, e.y)) {
                    trackStates[trackIdx].isArmed = !trackStates[trackIdx].isArmed;
                    if (auto* parent = findParentComponentOfClass<ArrangementView>()) {
                        if (parent->onTrackArmChanged) parent->onTrackArmChanged(trackIdx, trackStates[trackIdx].isArmed);
                    }
                    repaint();
                    return;
                }
                
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
                // Right click on the track row -> create/insert clip.
                // The automation overlay sits BELOW the row in its own area and
                // intercepts its own events, so no suppression needed here.
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

        void mouseDrag(const juce::MouseEvent& e) override {
            if (!rulerDragInProgress) return;
            double dragBar = pixelToBar(e.x);
            dragBar = std::max(1.0, dragBar);
            if (!e.mods.isShiftDown())
                dragBar = std::round(dragBar);

            double threshold = 0.5; // half-bar drag threshold before switching to loop mode
            if (std::abs(dragBar - rulerDragStart) >= threshold) {
                // Dragging — set loop region
                loopInBar  = std::min(rulerDragStart, dragBar);
                loopOutBar = std::max(rulerDragStart, dragBar);
                if (loopInBar < 1.0) loopInBar = 1.0;
                loopActive = true;
                repaint();
                if (auto* parent = findParentComponentOfClass<ArrangementView>())
                    if (parent->onLoopChanged) parent->onLoopChanged(loopInBar, loopOutBar);
            }
        }

        void mouseUp(const juce::MouseEvent&) override {
            rulerDragInProgress = false;
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

        // Returns the top-left Y pixel of track row 'trackIdx', accounting for
        // the ARR_AUTO_H gap inserted by the automation lane (if open).
        int trackIndexToY(int trackIdx) const
        {
            int autoTrack = -1;
            if (auto* parent = findParentComponentOfClass<ArrangementView>())
                if (parent->automationOverlay.isVisible())
                    autoTrack = parent->activeAutomationTrack;

            int runningY = ARR_HEADER_H;
            for (int t = 0; t < trackIdx; ++t) {
                runningY += ARR_SLOT_H;
                if (t == autoTrack)
                    runningY += ARR_AUTO_H;
            }
            return runningY;
        }

        void updateClipBounds() {
            for (auto* block : clipBlocks) {
                int x = barToPixel(block->clip.startBar);
                int w = static_cast<int>(block->clip.lengthBars * pixelsPerBar);
                int y = trackIndexToY(block->clip.trackIndex);
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

        // ── Loop region (UI state, mirrored to/from MainComponent) ────────────
        double loopInBar        = 1.0;
        double loopOutBar       = 5.0;
        bool   loopActive       = false;
        double rulerDragStart   = 1.0;
        bool   rulerDragInProgress = false;

        // ── DragAndDropTarget ──────────────────────────────────────────────────
        int dragOverTrackIndex = -1;

        bool isInterestedInDragSource (const SourceDetails& d) override
        {
            return d.description.toString().startsWith ("InstrumentDrag:") ||
                   d.description.toString().startsWith ("EffectDrag:")     ||
                   d.description.toString().startsWith ("PluginDrag:");
        }

        void itemDragEnter (const SourceDetails& d) override { updateContentDrag (d.localPosition); }
        void itemDragMove  (const SourceDetails& d) override { updateContentDrag (d.localPosition); }
        void itemDragExit  (const SourceDetails&)   override { if (dragOverTrackIndex != -1) { dragOverTrackIndex = -1; repaint(); } }

        void itemDropped (const SourceDetails& d) override
        {
            dragOverTrackIndex = -1;
            repaint();

            auto type = d.description.toString().fromFirstOccurrenceOf(":", false, false);
            int hit = yToTrackIndex(d.localPosition.y);

            if (auto* parent = findParentComponentOfClass<ArrangementView>()) {
                if (d.description.toString().startsWith("InstrumentDrag:")) {
                    if (parent->onInstrumentDropped) parent->onInstrumentDropped(hit, type);
                } else if (d.description.toString().startsWith("PluginDrag:")) {
                    juce::String path = d.description.toString().fromFirstOccurrenceOf("PluginDrag:", false, false);
                    if (parent->onInstrumentDropped) parent->onInstrumentDropped(hit, "__PluginPath__:" + path);
                } else if (d.description.toString().startsWith("EffectDrag:")) {
                    if (parent->onEffectDropped && hit >= 0) parent->onEffectDropped(hit, type);
                }
            }
        }

        void updateContentDrag (juce::Point<int> localPosition)
        {
            int newTrack = yToTrackIndex(localPosition.y);
            if (newTrack != dragOverTrackIndex) {
                dragOverTrackIndex = newTrack;
                repaint();
            }
        }
    };

    juce::Viewport     gridViewport;
    ArrangementContent content;
    int activeAutomationTrack = -1;
    int activeTakeLaneTrack   = -1;  // (3.2)

    double computeTotalBars() const
    {
        double maxBars = 16.0;
        for (auto* block : content.clipBlocks) {
            double endBar = block->clip.startBar + block->clip.lengthBars;
            if (endBar > maxBars) maxBars = endBar;
        }
        return maxBars + 8.0;
    }

    // Place the overlay as a dedicated lane BELOW the clip row so it is
    // never occluded by ArrClipBlock children.
    void positionAutomationOverlay()
    {
        if (activeAutomationTrack < 0 || !automationOverlay.isVisible()) return;
        // The automation lane sits directly below the track row
        int y = ARR_HEADER_H + activeAutomationTrack * ARR_SLOT_H + ARR_SLOT_H;
        int h = ARR_AUTO_H;
        automationOverlay.setBounds(ARR_TRACK_W, y, content.getWidth() - ARR_TRACK_W, h);
        automationOverlay.updateScale(computeTotalBars(), content.pixelsPerBar);
        // Ensure it is painted above any child components
        automationOverlay.toFront(false);
    }

    // (3.2) Place the take lane overlay below the track row (or below automation lane).
    void positionTakeLaneOverlay()
    {
        if (activeTakeLaneTrack < 0 || !takeLaneOverlay.isVisible()) return;
        int baseY = ARR_HEADER_H + activeTakeLaneTrack * ARR_SLOT_H + ARR_SLOT_H;
        // If automation overlay is open on the same track, stack beneath it
        if (activeAutomationTrack == activeTakeLaneTrack && automationOverlay.isVisible())
            baseY += ARR_AUTO_H;
        int h = takeLaneOverlay.requiredHeight();
        if (h <= 0) h = ARR_TAKE_LANE_H; // show at least one row height
        takeLaneOverlay.setBounds(ARR_TRACK_W, baseY, content.getWidth() - ARR_TRACK_W, h);
        takeLaneOverlay.updateScale(computeTotalBars(), content.pixelsPerBar);
        takeLaneOverlay.toFront(false);
    }

    void updateContentSize()
    {
        auto vb = gridViewport.getBounds();
        if (vb.isEmpty()) return;

        double maxBars = computeTotalBars();

        int numTracks = content.trackStates.empty() ? 1 : (int)content.trackStates.size();
        // When the automation lane is open, add its height so it doesn't get clipped
        int autoExtra  = (activeAutomationTrack >= 0 && automationOverlay.isVisible()) ? ARR_AUTO_H : 0;
        // (3.2) When take lane is open, add its height
        int takeExtra  = (activeTakeLaneTrack >= 0 && takeLaneOverlay.isVisible())
                             ? takeLaneOverlay.requiredHeight() : 0;
        int contentW = juce::jmax(vb.getWidth(), content.barToPixel(maxBars));
        int contentH = juce::jmax(vb.getHeight(), ARR_HEADER_H + numTracks * ARR_SLOT_H + autoExtra + takeExtra + 20);
        content.setSize(contentW, contentH);
        positionAutomationOverlay();
        positionTakeLaneOverlay();
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

    // ── Warp indicators (3.1) ───────────────────────────────────────────────
    if (clip.warpEnabled)
    {
        // Orange "W" badge in the top-right corner
        g.setColour(juce::Colour(0xffff9900));
        g.setFont(juce::Font(juce::FontOptions(9.0f)).boldened());
        g.drawText("W", getWidth() - 14, 2, 12, 12, juce::Justification::centred);

        // Draw warp marker diamonds along the top edge
        if (!clip.warpMarkers.empty())
        {
            g.setColour(juce::Colour(0xffffdd00));
            const double clipLenBeats = clip.lengthBars * 4.0;
            for (const auto& wm : clip.warpMarkers)
            {
                // Map targetBeat (0-based within clip) → pixel x
                double frac = (clipLenBeats > 0.0) ? (wm.targetBeat / clipLenBeats) : 0.0;
                float mx    = static_cast<float>(frac * getWidth());
                float my    = 4.0f;
                float sz    = 5.0f;
                juce::Path diamond;
                diamond.addTriangle(mx, my - sz, mx + sz, my, mx, my + sz);
                diamond.addTriangle(mx, my - sz, mx - sz, my, mx, my + sz);
                g.fillPath(diamond);
                // Vertical dashed tick line
                g.setColour(juce::Colour(0xffffdd00).withAlpha(0.4f));
                g.drawVerticalLine(static_cast<int>(mx), 0.0f, static_cast<float>(getHeight()));
                g.setColour(juce::Colour(0xffffdd00));
            }
        }
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
        m.addSeparator();

        // ── Warp section (3.1) ──────────────────────────────────────────────
        if (clip.warpEnabled)
            m.addItem(10, "Disable Warp");
        else
            m.addItem(11, "Enable Warp");

        m.addItem(12, "Set Clip BPM\u2026");

        // Warp Mode sub-menu (Complex active; Beats/Tones stubbed for v0.2)
        {
            juce::PopupMenu modeMenu;
            modeMenu.addItem(20, "Complex (Phase Vocoder)",  true,
                             clip.warpMode == ArrangementClip::WarpMode::Complex);
            modeMenu.addItem(21, "Beats (v0.2)" , false, false);
            modeMenu.addItem(22, "Tones (v0.2)" , false, false);
            m.addSubMenu("Warp Mode \u25b8", modeMenu);
        }

        m.addItem(13, "Add Warp Marker Here");
        m.addItem(14, "Clear Warp Markers");
        m.addSeparator();

        // ── Comping (3.2) ────────────────────────────────────────────────
        if (clip.takes.empty())
            m.addItem(30, "Show Take Lanes (no takes yet)", false);
        else
            m.addItem(30, "Show Take Lanes ("
                        + juce::String(clip.takes.size()) + " takes)");

        // Store the x position for marker placement
        float clickX = (float)e.x;

        m.showMenuAsync(juce::PopupMenu::Options(), [this, clickX](int result)
        {
            auto notify = [&]() {
                if (auto* parent = owner.findParentComponentOfClass<ArrangementView>())
                    if (parent->onClipMoved) parent->onClipMoved(&clip);
            };

            if (result == 1)
            {
                if (auto* parent = owner.findParentComponentOfClass<ArrangementView>())
                    if (parent->onClipDeleted) parent->onClipDeleted(&clip);
            }
            else if (result == 10) // Disable Warp
            {
                clip.warpEnabled = false;
                repaint();
                notify();
            }
            else if (result == 11) // Enable Warp
            {
                clip.warpEnabled = true;
                repaint();
                notify();
            }
            else if (result == 12) // Set Clip BPM
            {
                auto* alertWindow = new juce::AlertWindow(
                    "Set Clip BPM",
                    "Enter the original BPM of the audio material:",
                    juce::MessageBoxIconType::QuestionIcon);
                alertWindow->addTextEditor("bpm",
                    clip.clipBpm > 0.0 ? juce::String(clip.clipBpm, 2) : "120.0",
                    "BPM:");
                alertWindow->addButton("OK",     1, juce::KeyPress(juce::KeyPress::returnKey));
                alertWindow->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
                alertWindow->enterModalState(true, juce::ModalCallbackFunction::create(
                    [alertWindow, this, notify_copy = std::function<void()>(notify)](int btnResult) mutable
                    {
                        if (btnResult == 1)
                        {
                            double bpm = alertWindow->getTextEditorContents("bpm").getDoubleValue();
                            if (bpm > 0.0)
                            {
                                clip.clipBpm = bpm;
                                notify_copy();
                            }
                        }
                        delete alertWindow;
                    }), true);
            }
            else if (result == 20) // Warp Mode: Complex
            {
                clip.warpMode = ArrangementClip::WarpMode::Complex;
                notify();
            }
            else if (result == 13) // Add Warp Marker Here
            {
                // Convert click x to a beat position within the clip
                double clipLenBeats = clip.lengthBars * 4.0;
                double frac         = static_cast<double>(clickX) / getWidth();
                double targetBeat   = juce::jlimit(0.0, clipLenBeats, frac * clipLenBeats);
                // Place sourcePositionSeconds proportionally (will be adjusted by user later)
                double sourceSec    = (clip.clipBpm > 0.0)
                    ? (targetBeat / (clip.clipBpm / 60.0))
                    : (targetBeat * 0.5); // fallback: assume 120 BPM
                clip.warpMarkers.push_back({ sourceSec, targetBeat });
                std::sort(clip.warpMarkers.begin(), clip.warpMarkers.end(),
                    [](const WarpMarker& a, const WarpMarker& b) { return a.targetBeat < b.targetBeat; });
                repaint();
                notify();
            }
            else if (result == 14) // Clear Warp Markers
            {
                clip.warpMarkers.clear();
                repaint();
                notify();
            }
            else if (result == 30) // Show Take Lanes
            {
                if (auto* av = owner.findParentComponentOfClass<ArrangementView>())
                {
                    int trackIdx = clip.trackIndex;
                    if (trackIdx >= 0 && av->onShowTakeLanes)
                        av->onShowTakeLanes(trackIdx, &clip);
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
