#pragma once
#include <JuceHeader.h>
#include "MainComponent.h"
#include "ProjectManager.h"

// ════════════════════════════════════════════════════════════════════════════
// Phase 6.1: Global Project State Undo/Redo
// ════════════════════════════════════════════════════════════════════════════

class ProjectStateAction : public juce::UndoableAction
{
public:
    ProjectStateAction(MainComponent& mc, const juce::String& actionDesc) 
        : mainComp(mc), actionName(actionDesc), firstRun(true)
    {
        // Snapshot the state BEFORE the action happens
        mainComp.syncProjectToUI();
        stateBefore = mainComp.getProjectManager().getTree().createCopy();
    }

    // Called manually AFTER the action is performed in the UI to capture the "after" state
    void captureAfterState()
    {
        mainComp.syncProjectToUI();
        stateAfter = mainComp.getProjectManager().getTree().createCopy();
    }

    bool perform() override
    {
        if (firstRun) {
            firstRun = false;
            return true; // The UI already performed the action, do nothing
        }
        
        // This is a redo
        if (stateAfter.isValid()) {
            mainComp.getProjectManager().getTree() = stateAfter.createCopy();
            mainComp.syncUIToProject();
        }
        return true;
    }

    bool undo() override
    {
        if (stateBefore.isValid()) {
            mainComp.getProjectManager().getTree() = stateBefore.createCopy();
            mainComp.syncUIToProject();
        }
        return true;
    }

private:
    MainComponent& mainComp;
    juce::String actionName;
    bool firstRun;
    juce::ValueTree stateBefore;
    juce::ValueTree stateAfter;
};
