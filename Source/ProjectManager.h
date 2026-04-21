#pragma once
#include <JuceHeader.h>

class ProjectManager {
public:
    ProjectManager()
    {
        createNewProject();
    }

    void createNewProject()
    {
        projectTree = juce::ValueTree("Project");
        projectTree.setProperty("bpm", 120.0, nullptr);
        
        juce::ValueTree tracksTree("Tracks");
        projectTree.addChild(tracksTree, -1, nullptr);
    }

    bool saveProject(const juce::File& file)
    {
        if (auto xml = projectTree.createXml())
        {
            if (xml->writeTo(file, {}))
                return true;
        }
        return false;
    }

    bool loadProject(const juce::File& file)
    {
        if (auto xml = juce::XmlDocument::parse(file))
        {
            auto loadedTree = juce::ValueTree::fromXml(*xml);
            if (loadedTree.isValid() && loadedTree.hasType("Project"))
            {
                projectTree = loadedTree;
                return true;
            }
        }
        return false;
    }

    juce::ValueTree& getTree() { return projectTree; }

private:
    juce::ValueTree projectTree;
};
