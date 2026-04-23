#pragma once

#include <JuceHeader.h>
#include <atomic>
#include <array>

#include "LockFreeQueue.h"
#include "TrackCommand.h"
#include "GlobalTransport.h"
#include "PatternPool.h"
#include "MidiPattern.h"
#include "EuclideanPattern.h"
#include "TopBarComponent.h"
#include "BrowserComponent.h"
#include "SessionView.h"
#include "PatternEditor.h"
#include "DeviceView.h"
#include "ArrangementView.h"
#include "ProjectManager.h"
#include "../Instruments/InstrumentFactory.h"
#include "../Instruments/DrumRackComponent.h"
#include "Instrument.h"
#include "InstrumentFactory.h"
#include "../Effects/EffectProcessor.h"
#include "../Effects/EffectFactory.h"
#include "ClipData.h"
#include "AppAudioBuffer.h"
#include "ProjectManager.h"

struct Track
{
    std::atomic<float> gain     { 1.0f };
    std::atomic<float> sendALevel { 0.0f };
    std::atomic<float> rmsLevel { 0.0f };
    std::atomic<bool>  muted    { false };
    std::atomic<bool>  soloed   { false };

    LockFreeQueue<TrackCommand, 128> commandQueue;
    LockFreeQueue<Pattern*, 128>     garbageQueue;
    LockFreeQueue<InstrumentProcessor*, 8> instrumentGarbageQueue;
    std::atomic<InstrumentProcessor*> activeInstrument {nullptr};

    static constexpr int MAX_EFFECTS = 4;
    std::atomic<EffectProcessor*> effectChain[MAX_EFFECTS] {nullptr};
    LockFreeQueue<EffectProcessor*, 16> effectGarbageQueue;

    Pattern* currentPattern {nullptr};
    Pattern* pendingPattern {nullptr};
    double   switchSample   {-1.0};
    double   patternStartSample {-1.0};

    juce::MidiBuffer midiBuffer;

    void processPatternCommands(int64_t blockStartSample, int numSamples,
                                const GlobalTransport& transport)
    {
        // Clear first so that any AllNotesOff events injected below
        // by FlushNotes or StopPattern survive into the DSP step.
        midiBuffer.clear();

        while (auto optCmd = commandQueue.pop())
        {
            TrackCommand cmd = *optCmd;
            if (cmd.type == TrackCommand::Type::PlayPattern) {
                if (pendingPattern && pendingPattern != currentPattern) garbageQueue.push(pendingPattern);
                pendingPattern = cmd.patternPointer;
                switchSample   = cmd.scheduledSample;
            } else if (cmd.type == TrackCommand::Type::StopPattern) {
                if (cmd.scheduledSample <= 0) {
                    // Immediate stop — release all held voices before silencing
                    midiBuffer.addEvent(juce::MidiMessage::allNotesOff(1), 0);
                    if (currentPattern) garbageQueue.push(currentPattern);
                    if (pendingPattern && pendingPattern != currentPattern) garbageQueue.push(pendingPattern);
                    currentPattern = nullptr;
                    pendingPattern = nullptr;
                    switchSample   = -1.0;
                    patternStartSample = -1.0;
                } else {
                    if (pendingPattern && pendingPattern != currentPattern) garbageQueue.push(pendingPattern);
                    pendingPattern = nullptr;
                    switchSample   = cmd.scheduledSample;
                }
            } else if (cmd.type == TrackCommand::Type::FlushNotes) {
                // Inject AllNotesOff at the start of this block so any
                // currently-playing voices are released before the next
                // pattern (which may have removed notes) takes over.
                midiBuffer.addEvent(juce::MidiMessage::allNotesOff(1), 0);
            }
        }

        if (switchSample != -1.0 && switchSample >= blockStartSample && switchSample < blockStartSample + numSamples) {
            // Switch happens IN THIS BLOCK
            int samplesBefore = static_cast<int>(switchSample - blockStartSample);
            int samplesAfter  = numSamples - samplesBefore;

            if (currentPattern && samplesBefore > 0)
                currentPattern->getEventsForBuffer(midiBuffer, blockStartSample, samplesBefore, transport, patternStartSample);

            if (currentPattern && currentPattern != pendingPattern) 
                garbageQueue.push(currentPattern);
            
            midiBuffer.addEvent(juce::MidiMessage::allNotesOff(1), samplesBefore);
            midiBuffer.addEvent(juce::MidiMessage::allNotesOff(10), samplesBefore);

            currentPattern = pendingPattern;
            pendingPattern = nullptr;
            patternStartSample = switchSample;
            switchSample   = -1.0;

            if (currentPattern && samplesAfter > 0)
                currentPattern->getEventsForBuffer(midiBuffer, blockStartSample + samplesBefore, samplesAfter, transport, patternStartSample);
        } else {
            // No switch in this block, but check if we passed a switch that wasn't handled
            // (render thread missed the exact block due to OS scheduling jitter).
            if (switchSample != -1.0 && blockStartSample >= switchSample) {
                if (currentPattern && currentPattern != pendingPattern) garbageQueue.push(currentPattern);
                
                midiBuffer.addEvent(juce::MidiMessage::allNotesOff(1), 0);
                midiBuffer.addEvent(juce::MidiMessage::allNotesOff(10), 0);

                currentPattern = pendingPattern;
                pendingPattern = nullptr;
                patternStartSample = switchSample;
                switchSample   = -1.0;
                // BUG-2 FIX: generate events for the *current* block right now.
                // Without this, the first block after a late switch was always
                // silent — dropping the very first hit and introducing [0, blockSize]
                // samples of jitter relative to tracks that caught their switch
                // on-time.
                if (currentPattern)
                    currentPattern->getEventsForBuffer(midiBuffer, blockStartSample, numSamples, transport, patternStartSample);
            } else if (currentPattern) {
                currentPattern->getEventsForBuffer(midiBuffer, blockStartSample, numSamples, transport, patternStartSample);
            }
        }
    }

    void clear() {
        gain.store(1.0f, std::memory_order_relaxed);
        sendALevel.store(0.0f, std::memory_order_relaxed);
        rmsLevel.store(0.0f, std::memory_order_relaxed);
        muted.store(false, std::memory_order_relaxed);
        soloed.store(false, std::memory_order_relaxed);
        currentPattern = nullptr;
        pendingPattern = nullptr;
        switchSample = -1.0;
        patternStartSample = -1.0;
        midiBuffer.clear();
        while (commandQueue.pop()) {}
        while (garbageQueue.pop()) {}
        if (auto* old = activeInstrument.exchange(nullptr, std::memory_order_acq_rel)) {
            delete old; // Since clear() is usually called safely on the message thread during teardown/move
        }
        while (auto opt = instrumentGarbageQueue.pop()) { delete *opt; }

        for (int i = 0; i < MAX_EFFECTS; ++i) {
            if (auto* old = effectChain[i].exchange(nullptr, std::memory_order_acq_rel)) {
                delete old;
            }
        }
        while (auto opt = effectGarbageQueue.pop()) { delete *opt; }
    }

    void moveFrom(Track& other) {
        clear();
        gain.store(other.gain.load(std::memory_order_relaxed), std::memory_order_relaxed);
        sendALevel.store(other.sendALevel.load(std::memory_order_relaxed), std::memory_order_relaxed);
        rmsLevel.store(other.rmsLevel.load(std::memory_order_relaxed), std::memory_order_relaxed);
        muted.store(other.muted.load(std::memory_order_relaxed), std::memory_order_relaxed);
        soloed.store(other.soloed.load(std::memory_order_relaxed), std::memory_order_relaxed);
        currentPattern = other.currentPattern;
        pendingPattern = other.pendingPattern;
        switchSample = other.switchSample;
        patternStartSample = other.patternStartSample;
        midiBuffer = other.midiBuffer;
        while (auto opt = other.commandQueue.pop()) commandQueue.push(*opt);
        while (auto opt = other.garbageQueue.pop()) garbageQueue.push(*opt);
        other.currentPattern = nullptr;
        other.pendingPattern = nullptr;
        
        activeInstrument.store(other.activeInstrument.exchange(nullptr, std::memory_order_acq_rel), std::memory_order_release);
        while (auto opt = other.instrumentGarbageQueue.pop()) instrumentGarbageQueue.push(*opt);

        for (int i = 0; i < MAX_EFFECTS; ++i) {
            effectChain[i].store(other.effectChain[i].exchange(nullptr, std::memory_order_acq_rel), std::memory_order_release);
        }
        while (auto opt = other.effectGarbageQueue.pop()) effectGarbageQueue.push(*opt);
    }
};

struct OscTrack : public Track
{
    double currentAngle = 0.0;
    double angleDelta   = 0.0;
};

// ════════════════════════════════════════════════════════════════════════════
//  RenderThread — Non-RT thread that pre-renders DSP into AppAudioBuffer
// ════════════════════════════════════════════════════════════════════════════
class MainComponent;

class RenderThread : public juce::Thread
{
public:
    explicit RenderThread(MainComponent& mc) : juce::Thread("DAW Render Thread"), owner(mc) {}
    void run() override;

private:
    MainComponent& owner;
};

// ════════════════════════════════════════════════════════════════════════════
//  MainComponent
// ════════════════════════════════════════════════════════════════════════════
class MainComponent : public juce::AudioAppComponent,
                      public juce::MidiInputCallback,
                      public juce::Timer,
                      public juce::DragAndDropContainer
{
public:
    MainComponent();
    ~MainComponent() override;

    void updatePlayheadPhase();
    
    // UI Helpers
    void showDeviceEditorForTrack(int trackIdx);
    void regenerateDrumRackMidi(ClipData& clip);
    void regenerateEuclideanMidi(ClipData& clip);
    void loadAudioFileIntoDrumPad(int trackIdx, int padIndex, const juce::File& file);
    void updateDrumRackPatternEditor();
    
    void prepareToPlay (int samplesPerBlockExpected, double sampleRate) override;
    void getNextAudioBlock (const juce::AudioSourceChannelInfo& bufferToFill) override;
    void releaseResources() override;

    void paint   (juce::Graphics& g) override;
    void resized () override;
    bool keyPressed (const juce::KeyPress& key) override;

    void handleIncomingMidiMessage (juce::MidiInput* source,
                                    const juce::MidiMessage& message) override;
    void timerCallback() override;

    // ── State exposed to RenderThread (friend) ───────────────────────────────
    friend class RenderThread;

private:
    // ── Persistent Settings ──────────────────────────────────────────────────
    std::unique_ptr<juce::PropertiesFile> userSettings;
    juce::File workspaceDirectory;
    
    bool ensureWorkspaceDirectory();
    void initialiseWorkspace();
    void finishInitialisation();

    void saveDeviceSettings();
    void loadDeviceSettings();

    // ── Core state ───────────────────────────────────────────────────────────
    double currentSampleRate = 0.0;
    int    currentBufferSize = 512;
    GlobalTransport transportClock;
    ProjectManager  projectManager;
    juce::File      currentProjectFile;
    std::unique_ptr<juce::FileChooser> projectChooser;

    void configureProjectMenu();
    void syncProjectToUI();
    void syncUIToProject();
    void loadAudioFileIntoTrack(int trackIdx, const juce::File& file);
    void exportProject(const juce::File& outputFile, const juce::String& format);

    // ── Audio Graph Elements ─────────────────────────────────────────────────
    // Tracks are heap-allocated (Track has non-movable std::atomic members).
    // Pre-reserved to 128 so no reallocation occurs during the session
    // (vector pointer stability is required by the lock-free render thread).
    Track masterTrack;
    Track returnTrackA;
    std::vector<std::unique_ptr<Track>> audioTracks; // indexed 0..numActiveTracks-1
    std::atomic<int> numActiveTracks {0}; // written UI, read audio+render thread

    // ── Clip Grid State (UI thread only) ─────────────────────────────────────
    // clipGrid[t][s] — grows in sync with audioTracks
    std::vector<std::array<ClipData, NUM_SCENES>> clipGrid;
    int selectedTrackIndex = -1;
    int selectedSceneIndex = -1;
    int selectedDrumPadIndex = 0;

    // ── UI Elements ──────────────────────────────────────────────────────────
    std::unique_ptr<TopBarComponent> topBar;
    BrowserComponent browser;
    SessionView      sessionView;
    ArrangementView  arrangementView;
    DeviceView       deviceView;
    PatternEditor    patternEditor;

    enum class DAWView { Session, Arrangement };
    DAWView currentView { DAWView::Session };
    std::vector<std::vector<ArrangementClip>> arrangementTracks; // UI-thread only, one inner vec per track

    // ── Arrangement Engine (Thread-Safe) ─────────────────────────────────────
    std::atomic<SharedArrangement*> renderArrangement {nullptr};
    LockFreeQueue<SharedArrangement*, 16> arrangementGarbageQueue;
    std::atomic<bool> renderIsArrangementMode {false};
    std::atomic<int64_t> renderTransportOffset {0};
    
    std::atomic<bool> isExporting {false};

    void switchToView(DAWView v);
    void syncArrangementFromSession();

    // ── Level Meters ─────────────────────────────────────────────────────────
    float masterLevelDisplay = 0.0f;
    float audioLevelDisplay  = 0.0f;
    float returnLevelDisplay = 0.0f;

    // ── Instrument Engine ──────────────────────────────────────────────────
    std::vector<juce::String> trackInstruments; // Stores name of current instrument, indexed by track
    std::vector<juce::File>   loadedFiles;
    juce::AudioFormatManager formatManager;
    std::unique_ptr<juce::FileChooser> myChooser;

    // ── Pattern Pooling ──────────────────────────────────────────────────────
    PatternPool<MidiPattern>      midiPool;
    PatternPool<EuclideanPattern> euclideanPool;

    // ── Lock-Free Plugin Engine ──────────────────────────────────────────────
    juce::AudioPluginFormatManager   pluginFormatManager;
    juce::AudioPluginInstance*       activePlugin {nullptr}; // render-thread owned
    LockFreeQueue<juce::AudioPluginInstance*, 4> pluginLoadQueue;
    LockFreeQueue<juce::AudioPluginInstance*, 4> pluginGarbageQueue;
    std::unique_ptr<juce::DocumentWindow>        pluginWindow;

    // ── Lock-Free Hardware MIDI Input ────────────────────────────────────────
    juce::MidiMessageCollector midiCollector;

    // ── Application-Level Pre-Render Buffer ─────────────────────────────────
    // Decouples DSP work from the hard RT deadline of the hardware callback.
    // The render thread fills this asynchronously; getNextAudioBlock only reads.
    AppAudioBuffer appBuffer;
    RenderThread   renderThread;

    // Scratch buffer used by the render thread (pre-allocated in prepareToPlay)
    juce::AudioBuffer<float> renderScratch;

    // Signal from audio thread → render thread: wake up and fill the buffer
    juce::WaitableEvent renderWakeEvent { true }; // manual-reset

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
