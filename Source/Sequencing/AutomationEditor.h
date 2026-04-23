#pragma once
#include <JuceHeader.h>
#include "Pattern.h"

class AutomationEditorViewer : public juce::Component {
public:
    AutomationEditorViewer() {
        setOpaque(true);
    }

    void setAutomationLane(AutomationLane* lane, double lengthBeats) {
        currentLane = lane;
        patternLengthBeats = lengthBeats;
        repaint();
    }

    void setParameterId(const juce::String& paramId) {
        currentParameterId = paramId;
        repaint();
    }

    void setPlayheadPhase(float phase) {
        if (playheadPhase != phase) {
            playheadPhase = phase;
            repaint();
        }
    }

    void paint(juce::Graphics& g) override {
        g.fillAll(juce::Colour(0xff1e1e1e));

        g.setColour(juce::Colours::darkgrey);
        g.drawRect(getLocalBounds(), 1);

        if (currentParameterId.isEmpty()) {
            g.setColour(juce::Colours::grey);
            g.drawText("Touch a parameter to view its automation lane", getLocalBounds(), juce::Justification::centred);
            return;
        }

        // Draw grid
        g.setColour(juce::Colour(0xff2a2a2a));
        for (float i = 0.25f; i <= 1.0f; i += 0.25f) {
            g.drawHorizontalLine(static_cast<int>(getHeight() * i), 0, static_cast<float>(getWidth()));
        }
        for (int i = 1; i < std::ceil(patternLengthBeats); ++i) {
            float x = (i / static_cast<float>(patternLengthBeats)) * getWidth();
            g.drawVerticalLine(static_cast<int>(x), 0.0f, static_cast<float>(getHeight()));
        }

        // Title
        g.setColour(juce::Colours::white);
        g.drawText("Automation: " + currentParameterId, 5, 5, getWidth() - 10, 20, juce::Justification::topLeft);

        if (!currentLane) return;

        // Draw points and lines
        if (currentLane->points.size() > 0) {
            juce::Path path;
            for (size_t i = 0; i < currentLane->points.size(); ++i) {
                float x = static_cast<float>(currentLane->points[i].positionBeats / patternLengthBeats) * getWidth();
                float y = (1.0f - currentLane->points[i].value) * getHeight();
                
                if (i == 0) path.startNewSubPath(x, y);
                else path.lineTo(x, y);

                g.setColour(i == draggedPointIndex ? juce::Colours::white : juce::Colours::orange);
                g.fillEllipse(x - 4, y - 4, 8, 8);
            }
            g.setColour(juce::Colours::orange.withAlpha(0.6f));
            g.strokePath(path, juce::PathStrokeType(2.0f));
        }

        // Playhead
        if (playheadPhase >= 0.0f) {
            float px = playheadPhase * getWidth();
            g.setColour(juce::Colours::white.withAlpha(0.7f));
            g.drawVerticalLine(static_cast<int>(px), 0.0f, static_cast<float>(getHeight()));
        }
    }

    void mouseDown(const juce::MouseEvent& e) override {
        if (!currentLane) return;

        double beat = (e.position.x / getWidth()) * patternLengthBeats;
        float value = 1.0f - (e.position.y / getHeight());
        value = juce::jlimit(0.0f, 1.0f, value);

        // Check if we clicked an existing point
        draggedPointIndex = -1;
        for (size_t i = 0; i < currentLane->points.size(); ++i) {
            float px = static_cast<float>(currentLane->points[i].positionBeats / patternLengthBeats) * getWidth();
            float py = (1.0f - currentLane->points[i].value) * getHeight();
            if (e.position.getDistanceFrom({px, py}) < 10.0f) {
                draggedPointIndex = static_cast<int>(i);
                break;
            }
        }

        if (e.mods.isRightButtonDown()) {
            // Delete point
            if (draggedPointIndex >= 0) {
                bool isExtremeLeft = (draggedPointIndex == 0 && currentLane->points[0].positionBeats == 0.0);
                bool isExtremeRight = (draggedPointIndex == currentLane->points.size() - 1 && currentLane->points.back().positionBeats >= patternLengthBeats);
                if (!isExtremeLeft && !isExtremeRight) {
                    currentLane->points.erase(currentLane->points.begin() + draggedPointIndex);
                    if (onAutomationChanged) onAutomationChanged();
                }
                repaint();
            }
            return;
        }

        if (e.mods.isLeftButtonDown() && e.getNumberOfClicks() == 2 && draggedPointIndex == -1) {
            // Add point
            currentLane->points.push_back({ beat, value });
            std::sort(currentLane->points.begin(), currentLane->points.end(),
                      [](const AutomationPoint& a, const AutomationPoint& b) { return a.positionBeats < b.positionBeats; });
            if (onAutomationChanged) onAutomationChanged();
            repaint();
            return;
        }
    }

    void mouseDrag(const juce::MouseEvent& e) override {
        if (!currentLane || draggedPointIndex < 0) return;

        double beat = (e.position.x / getWidth()) * patternLengthBeats;
        
        float value = 1.0f - (e.position.y / getHeight());
        value = juce::jlimit(0.0f, 1.0f, value);

        bool isExtremeLeft = (draggedPointIndex == 0 && currentLane->points[0].positionBeats == 0.0);
        bool isExtremeRight = (draggedPointIndex == currentLane->points.size() - 1 && currentLane->points.back().positionBeats >= patternLengthBeats);

        if (isExtremeLeft) {
            beat = 0.0;
        } else if (isExtremeRight) {
            beat = patternLengthBeats;
        } else {
            beat = juce::jlimit(0.0, patternLengthBeats, beat);
        }

        currentLane->points[draggedPointIndex].positionBeats = beat;
        currentLane->points[draggedPointIndex].value = value;

        // Re-sort and find new index
        AutomationPoint draggedPoint = currentLane->points[draggedPointIndex];
        std::sort(currentLane->points.begin(), currentLane->points.end(),
                  [](const AutomationPoint& a, const AutomationPoint& b) { return a.positionBeats < b.positionBeats; });

        for (size_t i = 0; i < currentLane->points.size(); ++i) {
            if (currentLane->points[i].positionBeats == draggedPoint.positionBeats &&
                currentLane->points[i].value == draggedPoint.value) {
                draggedPointIndex = static_cast<int>(i);
                break;
            }
        }

        if (onAutomationChanged) onAutomationChanged();
        repaint();
    }

    void mouseUp(const juce::MouseEvent&) override {
        draggedPointIndex = -1;
        repaint();
    }

    std::function<void()> onAutomationChanged;

private:
    AutomationLane* currentLane = nullptr;
    juce::String currentParameterId;
    double patternLengthBeats = 4.0;
    float playheadPhase = -1.0f;
    int draggedPointIndex = -1;
};
