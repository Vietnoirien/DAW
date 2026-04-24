#pragma once
#include <JuceHeader.h>
#include "InstrumentFactory.h"

// ─── Instrument Tile ──────────────────────────────────────────────────────────
class InstrumentTile : public juce::Component
{
public:
    juce::String instrumentType;

    InstrumentTile (const juce::String& type, const juce::String& label, juce::Colour c)
        : instrumentType (type), displayLabel (label), tileColour (c) {}

    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat().reduced (4.0f);
        g.setColour (isHovered ? tileColour.brighter (0.25f) : tileColour.darker (0.15f));
        g.fillRoundedRectangle (b, 6.0f);

        g.setColour (juce::Colour (0x40ffffff));
        g.drawRoundedRectangle (b, 6.0f, 1.0f);

        g.setColour (juce::Colours::white);
        g.setFont (juce::Font (juce::FontOptions (13.0f, juce::Font::bold)));
        g.drawText (displayLabel, b.toNearestInt(), juce::Justification::centred);
    }

    void mouseEnter (const juce::MouseEvent&) override { isHovered = true;  repaint(); }
    void mouseExit  (const juce::MouseEvent&) override { isHovered = false; repaint(); }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (e.getDistanceFromDragStart() > 6)
            if (auto* c = juce::DragAndDropContainer::findParentDragContainerFor (this))
                c->startDragging ("InstrumentDrag:" + instrumentType, this);
    }

private:
    juce::String displayLabel;
    juce::Colour tileColour;
    bool         isHovered = false;
};

// ─── Effect Tile ──────────────────────────────────────────────────────────────
class EffectTile : public juce::Component
{
public:
    juce::String effectType;

    EffectTile (const juce::String& type, const juce::String& label, juce::Colour c)
        : effectType (type), displayLabel (label), tileColour (c) {}

    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat().reduced (4.0f);
        g.setColour (isHovered ? tileColour.brighter (0.25f) : tileColour.darker (0.15f));
        g.fillRoundedRectangle (b, 6.0f);

        g.setColour (juce::Colour (0x40ffffff));
        g.drawRoundedRectangle (b, 6.0f, 1.0f);

        g.setColour (juce::Colours::white);
        g.setFont (juce::Font (juce::FontOptions (13.0f, juce::Font::bold)));
        g.drawText (displayLabel, b.toNearestInt(), juce::Justification::centred);
    }

    void mouseEnter (const juce::MouseEvent&) override { isHovered = true;  repaint(); }
    void mouseExit  (const juce::MouseEvent&) override { isHovered = false; repaint(); }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (e.getDistanceFromDragStart() > 6)
            if (auto* c = juce::DragAndDropContainer::findParentDragContainerFor (this))
                c->startDragging ("EffectDrag:" + effectType, this);
    }

private:
    juce::String displayLabel;
    juce::Colour tileColour;
    bool         isHovered = false;
};

// ─── Plugin Tile ──────────────────────────────────────────────────────────────
// Represents a discovered VST3 / CLAP plugin in the Plugins browser tab.
// Dragging a tile sends "PluginDrag:<absolutePath>" to the drop target.
class PluginTile : public juce::Component
{
public:
    juce::String pluginPath;  // Absolute path to the plugin file / bundle
    juce::String displayName;

    PluginTile (const juce::String& path, const juce::String& name, juce::Colour c)
        : pluginPath (path), displayName (name), tileColour (c) {}

    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat().reduced (4.0f);
        g.setColour (isHovered ? tileColour.brighter (0.25f) : tileColour.darker (0.2f));
        g.fillRoundedRectangle (b, 6.0f);

        // Purple accent border
        g.setColour (isHovered ? juce::Colour (0xffaa77ff) : juce::Colour (0xff6633cc));
        g.drawRoundedRectangle (b, 6.0f, 1.5f);

        // Plugin icon + name
        g.setColour (juce::Colours::white.withAlpha (0.9f));
        g.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
        juce::String label = juce::String::fromUTF8 ("\xf0\x9f\x94\x8c") + "  " + displayName;
        g.drawText (label, b.reduced (4.0f).toNearestInt(), juce::Justification::centredLeft);
    }

    void mouseEnter (const juce::MouseEvent&) override { isHovered = true;  repaint(); }
    void mouseExit  (const juce::MouseEvent&) override { isHovered = false; repaint(); }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (e.getDistanceFromDragStart() > 6)
            if (auto* c = juce::DragAndDropContainer::findParentDragContainerFor (this))
                c->startDragging ("PluginDrag:" + pluginPath, this);
    }

private:
    juce::Colour tileColour { juce::Colour (0xff2a1a4a) };
    bool         isHovered  = false;
};


// ─── Instrument Browser Panel ─────────────────────────────────────────────────
class InstrumentBrowserPanel : public juce::Component
{
public:
    InstrumentBrowserPanel()
    {
        // ── Colour palette keyed by instrument name ──────────────────────────
        // Add an entry here whenever a new instrument is registered in
        // InstrumentFactory — the browser will pick it up automatically.
        static const std::map<juce::String, juce::Colour> kPalette {
            { "Simpler",        juce::Colour (0xff1a7a4a) },
            { "Oscillator",     juce::Colour (0xff1a4a7a) },
            { "DrumRack",       juce::Colour (0xff7a4a1a) },
            { "FMSynth",        juce::Colour (0xff8a5500) },
            { "WavetableSynth", juce::Colour (0xff006870) },
            { "KarplusStrong",  juce::Colour (0xff1a6a30) },
        };

        // ── Build tiles from the factory list ────────────────────────────────
        for (const auto& name : InstrumentFactory::getAvailableInstruments())
        {
            juce::Colour col = juce::Colour (0xff3a3a5a); // fallback
            if (kPalette.count (name))
                col = kPalette.at (name);

            // Pretty-print: insert spaces before capital letters (e.g. "FMSynth" → "FM Synth")
            juce::String label;
            for (int i = 0; i < name.length(); ++i)
            {
                if (i > 0 && juce::CharacterFunctions::isUpperCase (name[i])
                          && juce::CharacterFunctions::isLowerCase (name[i - 1]))
                    label += ' ';
                label += name[i];
            }

            auto* tile = new InstrumentTile (name, label, col);
            listContent.addAndMakeVisible (tile);
            tiles.add (tile);
        }

        // ── Viewport so the list scrolls as instruments are added ─────────────
        viewport.setViewedComponent (&listContent, false);
        viewport.setScrollBarsShown (true, false);
        addAndMakeVisible (viewport);
    }

    void resized() override
    {
        viewport.setBounds (getLocalBounds());

        const int tileH   = 44;
        const int spacing = 8;
        const int totalH  = spacing + tiles.size() * (tileH + spacing);

        listContent.setSize (getWidth() - viewport.getScrollBarThickness(), totalH);

        int y = spacing;
        for (auto* t : tiles)
        {
            t->setBounds (4, y, listContent.getWidth() - 8, tileH);
            y += tileH + spacing;
        }
    }

private:
    juce::Component                listContent;
    juce::Viewport                 viewport;
    juce::OwnedArray<InstrumentTile> tiles;
};



// ─── Effects Browser Panel ────────────────────────────────────────────────────
class EffectsBrowserPanel : public juce::Component
{
public:
    EffectsBrowserPanel()
    {
        addTile ("Reverb", "    Reverb", juce::Colour (0xff7a1a4a));
        addTile ("Delay", "    Delay", juce::Colour (0xff7a7a1a));
        addTile ("Chorus", "    Chorus", juce::Colour (0xff4a7a1a));
        addTile ("Filter", "    Filter", juce::Colour (0xff4a1a7a));
        addTile ("Compressor", "  Compressor", juce::Colour (0xffaa4422));
        addTile ("Limiter", "    Limiter", juce::Colour (0xffdd3322));
        addTile ("Phaser", "    Phaser", juce::Colour (0xff22aa77));
        addTile ("Saturation", "  Saturation", juce::Colour (0xffcc7722));
        addTile ("ParametricEQ", " Parametric EQ", juce::Colour (0xff443399));
    }

    void resized() override
    {
        int y = 8;
        for (auto* t : tiles) { t->setBounds (4, y, getWidth() - 8, 44); y += 52; }
    }

private:
    void addTile (const juce::String& type, const juce::String& label, juce::Colour c)
    {
        auto* t = new EffectTile (type, label, c);
        addAndMakeVisible (t);
        tiles.add (t);
    }

    juce::OwnedArray<EffectTile> tiles;
};

// ─── Plugins Browser Panel ────────────────────────────────────────────────────
// Scans a list of user-managed directories for VST3 and CLAP plugin files.
// Each discovered plugin is shown as a draggable PluginTile.
class PluginsBrowserPanel : public juce::Component
{
public:
    // Called when the user clicks "Add Folder…"
    std::function<void(const juce::File&)> onAddFolder;
    // Called when the user clicks "Rescan"
    std::function<void()>                  onRescan;

    PluginsBrowserPanel()
    {
        addFolderBtn.setButtonText (juce::String::fromUTF8 ("Add Folder\xe2\x80\xa6"));
        addFolderBtn.onClick = [this]
        {
            fileChooser = std::make_unique<juce::FileChooser> ("Select a plugin folder…",
                                                               juce::File(),
                                                               "");
            auto flags = juce::FileBrowserComponent::openMode
                       | juce::FileBrowserComponent::canSelectDirectories;
            fileChooser->launchAsync (flags, [this](const juce::FileChooser& fc)
            {
                auto result = fc.getResult();
                if (result.isDirectory() && onAddFolder)
                    onAddFolder (result);
            });
        };
        addAndMakeVisible (addFolderBtn);

        rescanBtn.setButtonText (juce::String::fromUTF8 ("\xe2\x86\xbb  Rescan"));
        rescanBtn.onClick = [this] { if (onRescan) onRescan(); };
        addAndMakeVisible (rescanBtn);

        viewport.setViewedComponent (&listContent, false);
        viewport.setScrollBarsShown (true, false);
        addAndMakeVisible (viewport);

        styleButton (addFolderBtn, juce::Colour (0xff3a1a6a));
        styleButton (rescanBtn,    juce::Colour (0xff1a3a6a));
    }

    // ── Public API ────────────────────────────────────────────────────────────

    void addSearchPath (const juce::File& dir)
    {
        if (!searchPaths.contains (dir.getFullPathName()))
            searchPaths.add (dir.getFullPathName());
    }

    juce::StringArray getSearchPaths() const { return searchPaths; }

    void setSearchPaths (const juce::StringArray& paths) { searchPaths = paths; }

    // Rebuild tiles from a PluginDescription array produced by a scan.
    void setPlugins (const juce::Array<juce::PluginDescription>& descs)
    {
        tiles.clear();
        listContent.removeAllChildren();

        // Alternate two purple shades so adjacent tiles are distinguishable
        const juce::Colour kColA (0xff2a1a4a);
        const juce::Colour kColB (0xff1a2a4a);

        for (int i = 0; i < descs.size(); ++i)
        {
            const auto& d = descs.getReference (i);
            auto* tile = new PluginTile (d.fileOrIdentifier, d.name,
                                          i % 2 == 0 ? kColA : kColB);
            listContent.addAndMakeVisible (tile);
            tiles.add (tile);
        }
        resized();
        repaint();
    }

    void resized() override
    {
        auto area = getLocalBounds();
        auto btnRow = area.removeFromTop (34);
        addFolderBtn.setBounds (btnRow.removeFromLeft (btnRow.getWidth() / 2).reduced (3));
        rescanBtn.setBounds    (btnRow.reduced (3));

        viewport.setBounds (area);

        const int tileH   = 44;
        const int spacing = 6;
        const int totalH  = spacing + tiles.size() * (tileH + spacing);
        listContent.setSize (area.getWidth() - viewport.getScrollBarThickness(),
                             juce::jmax (area.getHeight(), totalH));

        int y = spacing;
        for (auto* t : tiles)
        {
            t->setBounds (4, y, listContent.getWidth() - 8, tileH);
            y += tileH + spacing;
        }
    }

    void paint (juce::Graphics& g) override
    {
        if (tiles.isEmpty())
        {
            g.setColour (juce::Colours::grey);
            g.setFont (juce::Font (juce::FontOptions (12.0f)));
            g.drawText (juce::String::fromUTF8 (u8"No plugins found.\nClick \u201cAdd Folder\u2026\u201d to add a plugin directory."),
                        getLocalBounds().reduced (12).withTrimmedTop (40),
                        juce::Justification::centredTop, true);
        }
    }

private:
    static void styleButton (juce::TextButton& b, juce::Colour bg)
    {
        b.setColour (juce::TextButton::buttonColourId,    bg);
        b.setColour (juce::TextButton::buttonOnColourId,  bg.brighter (0.3f));
        b.setColour (juce::TextButton::textColourOffId,   juce::Colours::white);
    }

    juce::TextButton addFolderBtn, rescanBtn;
    juce::Component  listContent;
    juce::Viewport   viewport;
    juce::OwnedArray<PluginTile> tiles;
    juce::StringArray searchPaths;
    std::unique_ptr<juce::FileChooser> fileChooser;
};

// ─── File Browser Panel ───────────────────────────────────────────────────────
class FileBrowserPanel : public juce::Component
{
public:
    std::function<void(const juce::File&)> onFolderSelected;

    FileBrowserPanel (juce::DirectoryContentsList& listToUse, juce::FileTreeComponent& treeToUse)
        : directoryList(listToUse), fileTree(treeToUse)
    {
        chooseFolderButton.setButtonText ("Choose Folder...");
        chooseFolderButton.onClick = [this] {
            fileChooser = std::make_unique<juce::FileChooser> ("Select a folder to browse...",
                                                               juce::File(),
                                                               "");
            auto folderChooserFlags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories;
            fileChooser->launchAsync (folderChooserFlags, [this] (const juce::FileChooser& fc)
            {
                auto result = fc.getResult();
                if (result.isDirectory())
                {
                    directoryList.setDirectory (result, true, true);
                    if (onFolderSelected)
                        onFolderSelected (result);
                }
            });
        };
        addAndMakeVisible (chooseFolderButton);
        addAndMakeVisible (fileTree);
    }

    void resized() override
    {
        auto b = getLocalBounds();
        chooseFolderButton.setBounds (b.removeFromTop (32).reduced (4));
        fileTree.setBounds (b);
    }

private:
    juce::DirectoryContentsList& directoryList;
    juce::FileTreeComponent& fileTree;
    juce::TextButton chooseFolderButton;
    std::unique_ptr<juce::FileChooser> fileChooser;
};

// ─── Browser Component ────────────────────────────────────────────────────────
class BrowserComponent : public juce::Component
{
public:
    std::function<void(const juce::File&)> onFolderSelected;

    BrowserComponent()
        : thread       ("BrowserThread"),
          fileFilter   ("*.wav;*.aiff;*.flac;*.mp3;*.ogg", "*", "Audio Files"),
          directoryList (&fileFilter, thread),
          fileTree      (directoryList),
          filesPanel    (directoryList, fileTree)
    {
        thread.startThread (juce::Thread::Priority::background);
        directoryList.setDirectory (juce::File(), true, true);
        fileTree.setDragAndDropDescription ("FileBrowserDrag");

        tabs = std::make_unique<juce::TabbedComponent> (juce::TabbedButtonBar::TabsAtTop);
        tabs->setTabBarDepth (28);
        tabs->setColour (juce::TabbedComponent::backgroundColourId,      juce::Colour (0xff0d0d1a));
        tabs->setColour (juce::TabbedComponent::outlineColourId,         juce::Colour (0xff252540));
        tabs->addTab ("Files",       juce::Colour (0xff0d0d1a), &filesPanel,         false);
        tabs->addTab ("Instruments", juce::Colour (0xff0d0d1a), &instrumentPanel,  false);
        tabs->addTab ("Effects",     juce::Colour (0xff0d0d1a), &effectsPanel,     false);
        tabs->addTab ("Plugins",     juce::Colour (0xff0d0d1a), &pluginsPanel,     false);
        addAndMakeVisible (tabs.get());
        setOpaque (true);

        filesPanel.onFolderSelected = [this] (const juce::File& f) {
            if (onFolderSelected)
                onFolderSelected (f);
        };
    }

    void setDirectory (const juce::File& d) { directoryList.setDirectory (d, true, true); }
    juce::File getDirectory() const         { return directoryList.getDirectory(); }

    ~BrowserComponent() override { thread.stopThread (1000); }

    void paint   (juce::Graphics& g) override { g.fillAll (juce::Colour (0xff0a0a14)); }
    void resized ()                  override { tabs->setBounds (getLocalBounds()); }

    // ── Plugin-browser helpers (called from MainComponent) ─────────────────
    void addPluginSearchPath (const juce::File& dir)
    {
        pluginsPanel.addSearchPath (dir);
    }

    void setPluginSearchPaths (const juce::StringArray& paths)
    {
        pluginsPanel.setSearchPaths (paths);
    }

    juce::StringArray getPluginSearchPaths() const
    {
        return pluginsPanel.getSearchPaths();
    }

    // Runs a synchronous scan across all registered paths and repopulates
    // the tiles.  Call from the message thread only.
    void rescanPlugins (juce::AudioPluginFormatManager& formatManager)
    {
        juce::Array<juce::PluginDescription> found;

        for (const auto& pathStr : pluginsPanel.getSearchPaths())
        {
            juce::File dir (pathStr);
            if (!dir.isDirectory()) continue;

            // Collect .vst3 bundles (directories on Linux) and .clap files
            juce::Array<juce::File> candidates;
            dir.findChildFiles (candidates, juce::File::findFilesAndDirectories, true,
                                "*.vst3;*.clap");

            for (auto& candidate : candidates)
            {
                for (auto* fmt : formatManager.getFormats())
                {
                    if (!fmt->fileMightContainThisPluginType (candidate.getFullPathName()))
                        continue;

                    juce::OwnedArray<juce::PluginDescription> results;
                    fmt->findAllTypesForFile (results, candidate.getFullPathName());

                    for (auto* d : results)
                    {
                        // Avoid duplicates (same file scanned by multiple formats)
                        bool duplicate = false;
                        for (const auto& existing : found)
                            if (existing.fileOrIdentifier == d->fileOrIdentifier
                                && existing.name == d->name)
                            { duplicate = true; break; }

                        if (!duplicate)
                            found.add (*d);
                    }
                }
            }
        }

        pluginsPanel.setPlugins (found);
    }

    // Forward callbacks from pluginsPanel so MainComponent can wire them
    PluginsBrowserPanel& getPluginsPanel() { return pluginsPanel; }

private:
    juce::TimeSliceThread        thread;
    juce::WildcardFileFilter     fileFilter;
    juce::DirectoryContentsList  directoryList;
    juce::FileTreeComponent      fileTree;
    FileBrowserPanel             filesPanel;
    InstrumentBrowserPanel       instrumentPanel;
    EffectsBrowserPanel          effectsPanel;
    PluginsBrowserPanel          pluginsPanel;
    std::unique_ptr<juce::TabbedComponent> tabs;
};
