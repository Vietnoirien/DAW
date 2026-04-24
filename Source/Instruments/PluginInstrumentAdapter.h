#pragma once
#include <JuceHeader.h>
#include "Instrument.h"

// ─── PluginInstrumentAdapter ──────────────────────────────────────────────────
// Wraps a JUCE AudioPluginInstance so it can be used as an InstrumentProcessor
// in the per-track audio graph.
//
// Thread-safety contract
// ──────────────────────
//  * processBlock() is called on the render thread.
//  * createEditor() / destructor are called on the message thread.
//  * When the instrument is replaced the render thread pops it from
//    instrumentGarbageQueue and posts "delete this" via callAsync, so the
//    destructor always runs on the message thread.
//  * The PluginPlaceholder component (returned by createEditor) holds only a
//    shared_ptr<function<void()>> so it can never dangle-reference this adapter
//    after the adapter is deleted — the shared_ptr just becomes the sole owner
//    and the function inside it becomes a no-op when the adapter nulls the ptr.
// ─────────────────────────────────────────────────────────────────────────────
class PluginInstrumentAdapter : public InstrumentProcessor
{
public:
    explicit PluginInstrumentAdapter (std::unique_ptr<juce::AudioPluginInstance> inst,
                                      const juce::String& pluginName)
        : plugin (std::move (inst)), name (pluginName) {}

    ~PluginInstrumentAdapter() override
    {
        // Always on message thread (see thread-safety contract above).
        //
        // 1. Null-out the shared toggle so any surviving PluginPlaceholder
        //    components become safe no-ops when their button is clicked.
        if (auto p = weakToggle.lock())
            *p = nullptr;   // replace the callable with an empty function

        // 2. Destroy the plugin window before releasing the plugin.
        pluginWindow.reset();

        // 3. Delay the destruction of the plugin processor using a timer.
        //    This gives the VST3 plugin's background threads time (200ms) to process
        //    the editor's closure (which might require the message thread) before we
        //    actually delete the processor. This completely avoids deadlocks.
        if (plugin)
        {
            auto* rawPlugin = plugin.release();
            juce::Timer::callAfterDelay (200, [rawPlugin] {
                rawPlugin->releaseResources();
                delete rawPlugin;
            });
        }
    }

    // ── InstrumentProcessor interface ─────────────────────────────────────────
    void prepareToPlay (double sampleRate) override
    {
        if (plugin)
            plugin->prepareToPlay (sampleRate, 512);
    }

    void processBlock (juce::AudioBuffer<float>& buffer, const juce::MidiBuffer& midi) override
    {
        if (plugin)
        {
            juce::MidiBuffer mutableMidi (midi);
            plugin->processBlock (buffer, mutableMidi);
        }
    }

    void clear() override
    {
        if (plugin)
            plugin->reset();
    }

    juce::ValueTree saveState() const override
    {
        juce::ValueTree v ("PluginInstrument");
        v.setProperty ("pluginName", name, nullptr);
        return v;
    }

    void loadState (const juce::ValueTree&) override {}

    void registerAutomationParameters (AutomationRegistry*) override {}

    juce::String getName() const override { return name; }

    void closeUI() override
    {
        pluginWindow.reset();
    }

    // ── Editor (placeholder tile in DeviceView) ───────────────────────────────
    std::unique_ptr<juce::Component> createEditor() override
    {
        // Share ownership of the toggle callable via shared_ptr so the
        // PluginPlaceholder never holds a raw this-pointer.
        auto shared = std::make_shared<std::function<void()>> (
            [this] { togglePluginWindow(); });
        weakToggle = shared;   // adapter keeps a weak handle to null it on death
        return std::make_unique<PluginPlaceholder> (name, std::move (shared));
    }

    // ── Plugin window ─────────────────────────────────────────────────────────
    void togglePluginWindow()
    {
        if (pluginWindow != nullptr)
        {
            pluginWindow->setVisible (! pluginWindow->isVisible());
            return;
        }
        openPluginWindow();
    }

    bool isWindowVisible() const
    {
        return pluginWindow != nullptr && pluginWindow->isVisible();
    }

private:
    // ── Placeholder tile shown in the DeviceView chain ────────────────────────
    struct PluginPlaceholder : public juce::Component
    {
        juce::String  pluginName;
        // Shared ownership: if the adapter is deleted first, it sets *toggle = nullptr
        // so clicking the button becomes a safe no-op.
        std::shared_ptr<std::function<void()>> toggle;
        juce::TextButton toggleBtn { "Show / Hide Editor" };

        PluginPlaceholder (const juce::String& n,
                           std::shared_ptr<std::function<void()>> t)
            : pluginName (n), toggle (std::move (t))
        {
            addAndMakeVisible (toggleBtn);
            toggleBtn.onClick = [this]
            {
                if (toggle && *toggle)
                    (*toggle)();
            };
            setSize (200, 80);
        }

        void paint (juce::Graphics& g) override
        {
            auto b = getLocalBounds().toFloat().reduced (4.0f);
            g.setColour (juce::Colour (0xff1a1a2e));
            g.fillRoundedRectangle (b, 6.0f);
            g.setColour (juce::Colour (0xff7b4fcf));
            g.drawRoundedRectangle (b, 6.0f, 1.5f);
            g.setColour (juce::Colours::white);
            g.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
            g.drawText (juce::String::fromUTF8 ("\xf0\x9f\x94\x8c") + " " + pluginName,
                        b.removeFromTop (30).toNearestInt(), juce::Justification::centred);
        }

        void resized() override
        {
            toggleBtn.setBounds (getLocalBounds().reduced (8).removeFromBottom (28));
        }
    };

    void openPluginWindow()
    {
        if (plugin == nullptr || !plugin->hasEditor())
            return;

        auto* editor = plugin->createEditorIfNeeded();
        if (editor == nullptr)
            return;

        pluginWindow = std::make_unique<juce::DocumentWindow> (
            name + " \xe2\x80\x94 Plugin Editor",
            juce::Colour (0xff1a1a2e),
            juce::DocumentWindow::closeButton);
        pluginWindow->setContentOwned (editor, true);
        pluginWindow->setResizable (true, false);
        pluginWindow->centreWithSize (editor->getWidth(), editor->getHeight());
        pluginWindow->setVisible (true);
        pluginWindow->toFront (true);
    }

    std::unique_ptr<juce::AudioPluginInstance>          plugin;
    std::unique_ptr<juce::DocumentWindow>               pluginWindow;
    juce::String                                        name;
    // Weak handle to the shared toggle callable — nulled in destructor.
    std::weak_ptr<std::function<void()>>                weakToggle;
};
