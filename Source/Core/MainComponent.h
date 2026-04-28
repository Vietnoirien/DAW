#pragma once

#include <JuceHeader.h>
#include <atomic>
#include <array>
#include <unordered_map>

#include "LockFreeQueue.h"
#include "TrackCommand.h"
#include "MpeZoneManager.h"
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
#include "../Instruments/PluginInstrumentAdapter.h"
#include "InstrumentFactory.h"
#include "../Effects/EffectProcessor.h"
#include "../Effects/EffectFactory.h"
#include "ClipData.h"
#include "AppAudioBuffer.h"
#include "ProjectManager.h"
#include "AudioClipPlayer.h"
#include "CompPlayer.h"
#include "../ControlSurface/ControlSurfaceManager.h"
#include "../ControlSurface/ApcMiniDriver.h"

// ─────────────────────────────────────────────────────────────────────────────
// PdcDelayLine — lock-free stereo ring-buffer delay used by the render thread
// to time-align tracks that have lower latency than the slowest effect chain.
// Written by the message thread (setDelay), read/processed by the render thread.
// ─────────────────────────────────────────────────────────────────────────────
struct PdcDelayLine
{
    // 8 192 samples ≈ 170 ms @ 48 kHz — well beyond any real plugin lookahead.
    static constexpr int kMaxDelay = 8192;
    std::array<float, kMaxDelay> bufL {}, bufR {};
    int writePos      = 0;
    int delaySamples  = 0;

    void setDelay (int d)
    {
        delaySamples = juce::jlimit (0, kMaxDelay - 1, d);
        bufL.fill (0.0f);
        bufR.fill (0.0f);
        writePos = 0;
    }

    void process (juce::AudioBuffer<float>& buf)
    {
        if (delaySamples == 0) return;
        const int n  = buf.getNumSamples();
        float*    L  = buf.getWritePointer (0);
        float*    R  = buf.getNumChannels() > 1 ? buf.getWritePointer (1) : nullptr;
        for (int i = 0; i < n; ++i)
        {
            const int readPos = (writePos - delaySamples + kMaxDelay) % kMaxDelay;
            const float outL  = bufL[readPos];
            bufL[writePos]    = L[i];
            L[i]              = outL;
            if (R)
            {
                const float outR = bufR[readPos];
                bufR[writePos]   = R[i];
                R[i]             = outR;
            }
            writePos = (writePos + 1) % kMaxDelay;
        }
    }
};

struct Track : public AutomationRegistry
{
    std::atomic<float> gain     { 1.0f };
    std::array<std::atomic<float>, 8> sendLevels;
    
    Track() {
        for (auto& s : sendLevels) s.store(0.0f);
    }
    std::atomic<float> rmsLevel { 0.0f };
    std::atomic<bool>  muted    { false };
    std::atomic<bool>  soloed   { false };

    // ── Plugin Delay Compensation ──
    // Written by the message thread (recalculatePDC), read lock-free by render.
    std::atomic<int> pdcDelaySamples { 0 };
    PdcDelayLine     pdcLine;

    // ── Sidechain source (2.1) ──────────────────────────────────────────────
    // Index into audioTracks[] whose pre-rendered output is used as the
    // sidechain key for any sidechain-capable effect on THIS track.
    // −1 = no sidechain.
    std::atomic<int> sidechainSourceTrack { -1 };

    // ── Group bus routing (2.2) ─────────────────────────────────────────────
    // Index into groupTracks[]. −1 = route to master (default).
    // Written by the message thread (right-click routing), read by render thread.
    std::atomic<int> groupBusIndex { -1 };
    // ── Recording ──
    std::atomic<bool>  isArmedForRecord { false };
    std::atomic<int>   recordInputChannel { 0 };
    std::unique_ptr<juce::AbstractFifo> recordFifo;
    std::vector<float> recordBuffer;
    
    // ── Monitoring ──
    std::atomic<bool>  monitorEnabled { false };
    std::atomic<float> monitorGain { 1.0f };
    std::unique_ptr<juce::AbstractFifo> monitorFifo;
    std::vector<float> monitorBuffer;

    // ── Audio clip playback + warping (3.1) ──────────────────────────────────
    // Owned entirely on the message thread (load/unload). The render thread
    // calls fillBlock() which is fully lock-free.
    std::unique_ptr<AudioClipPlayer> clipPlayer;
    std::atomic<bool> clipPlayerActive { false };

    // ── Comping (3.2) ────────────────────────────────────────────────────────
    // Owned on the message thread; render thread calls fillBlock() lock-free.
    // Active when compPlayerActive == true AND the clip has takes.size() > 0.
    // Takes priority over clipPlayer / warpEnabled when active.
    std::unique_ptr<CompPlayer> compPlayer;
    std::atomic<bool>           compPlayerActive { false };

    juce::File         currentRecordingFile;
    std::unique_ptr<juce::AudioFormatWriter> recordWriter;
    bool               wasRecording { false }; // Used by RecordingThread
    int64_t            recordStartSample { -1 };

    LockFreeQueue<TrackCommand, 128> commandQueue;
    LockFreeQueue<Pattern*, 128>     garbageQueue;
    LockFreeQueue<InstrumentProcessor*, 8> instrumentGarbageQueue;
    std::atomic<InstrumentProcessor*> activeInstrument {nullptr};

    std::atomic<std::vector<EffectProcessor*>*> activeEffectChain {nullptr};
    LockFreeQueue<EffectProcessor*, 16> effectGarbageQueue;
    LockFreeQueue<std::vector<EffectProcessor*>*, 16> effectVectorGarbageQueue;

    Pattern* currentPattern {nullptr};
    Pattern* pendingPattern {nullptr};
    double   switchSample   {-1.0};
    double   patternStartSample {-1.0};

    juce::MidiBuffer midiBuffer;
    
    // ── Parameter Automation Registry ──
    struct AutomatableParameter {
        std::atomic<float>* ptr;
        float minVal;
        float maxVal;
    };
    juce::SpinLock automationLock;
    std::unordered_map<juce::String, AutomatableParameter> automationRegistry;
    
    void registerParameter(const juce::String& id, std::atomic<float>* ptr, float minVal = 0.0f, float maxVal = 1.0f) override {
        juce::SpinLock::ScopedLockType sl(automationLock);
        if (ptr) automationRegistry[id] = {ptr, minVal, maxVal};
    }

    void refreshAutomationRegistry() {
        {
            juce::SpinLock::ScopedLockType sl(automationLock);
            automationRegistry.clear();
        }
        if (auto* inst = activeInstrument.load(std::memory_order_acquire)) {
            inst->registerAutomationParameters(this);
        }
        if (auto* vec = activeEffectChain.load(std::memory_order_acquire)) {
            for (auto* effect : *vec) {
                if (effect) effect->registerAutomationParameters(this);
            }
        }
    }

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
            } else if (cmd.type == TrackCommand::Type::AuditionNoteOn) {
                midiBuffer.addEvent(juce::MidiMessage::noteOn(1, cmd.note, (juce::uint8)cmd.velocity), 0);
            } else if (cmd.type == TrackCommand::Type::AuditionNoteOff) {
                midiBuffer.addEvent(juce::MidiMessage::noteOff(1, cmd.note, (juce::uint8)0), 0);
            } else if (cmd.type == TrackCommand::Type::MpeExpression) {
                // ── 4.1 MPE: inject per-note expression as standard MIDI ────────
                // Using the member channel from the payload so per-voice filtering
                // inside the instrument's processBlock works without modification.
                const auto& mpe = cmd.mpe;
                const int ch = juce::jlimit(1, 15, mpe.channel);

                // Pitch bend: ±48 semitones → 0..16383 (centre = 8192)
                int pbRaw = (int)((mpe.pitchBendSemitones / 48.0f * 0.5f + 0.5f) * 16383.0f);
                pbRaw = juce::jlimit(0, 16383, pbRaw);
                midiBuffer.addEvent(juce::MidiMessage::pitchWheel(ch, pbRaw), 0);

                // Channel pressure (0-1 → 0-127)
                int cp = juce::jlimit(0, 127, (int)(mpe.pressure * 127.0f));
                midiBuffer.addEvent(juce::MidiMessage::channelPressureChange(ch, cp), 0);

                // Timbre / slide: CC 74 (0-1 → 0-127)
                int cc74 = juce::jlimit(0, 127, (int)(mpe.timbre * 127.0f));
                midiBuffer.addEvent(juce::MidiMessage::controllerEvent(ch, 74, cc74), 0);
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
        for (auto& s : sendLevels) s.store(0.0f, std::memory_order_relaxed);
        rmsLevel.store(0.0f, std::memory_order_relaxed);
        muted.store(false, std::memory_order_relaxed);
        soloed.store(false, std::memory_order_relaxed);
        isArmedForRecord.store(false, std::memory_order_relaxed);
        sidechainSourceTrack.store(-1, std::memory_order_relaxed);
        groupBusIndex.store(-1, std::memory_order_relaxed);
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

        if (auto* oldVec = activeEffectChain.exchange(nullptr, std::memory_order_acq_rel)) {
            for (auto* old : *oldVec) {
                if (old) delete old;
            }
            delete oldVec;
        }
        while (auto opt = effectGarbageQueue.pop()) { delete *opt; }
        while (auto opt = effectVectorGarbageQueue.pop()) { delete *opt; }
    }

    void moveFrom(Track& other) {
        clear();
        gain.store(other.gain.load(std::memory_order_relaxed), std::memory_order_relaxed);
        for (size_t i = 0; i < sendLevels.size(); ++i)
            sendLevels[i].store(other.sendLevels[i].load(std::memory_order_relaxed), std::memory_order_relaxed);
        rmsLevel.store(other.rmsLevel.load(std::memory_order_relaxed), std::memory_order_relaxed);
        muted.store(other.muted.load(std::memory_order_relaxed), std::memory_order_relaxed);
        soloed.store(other.soloed.load(std::memory_order_relaxed), std::memory_order_relaxed);
        isArmedForRecord.store(other.isArmedForRecord.load(std::memory_order_relaxed), std::memory_order_relaxed);
        monitorEnabled.store(other.monitorEnabled.load(std::memory_order_relaxed), std::memory_order_relaxed);
        monitorGain.store(other.monitorGain.load(std::memory_order_relaxed), std::memory_order_relaxed);
        recordInputChannel.store(other.recordInputChannel.load(std::memory_order_relaxed), std::memory_order_relaxed);
        sidechainSourceTrack.store(other.sidechainSourceTrack.load(std::memory_order_relaxed), std::memory_order_relaxed);
        groupBusIndex.store(other.groupBusIndex.load(std::memory_order_relaxed), std::memory_order_relaxed);
        recordFifo = std::move(other.recordFifo);
        recordBuffer = std::move(other.recordBuffer);
        monitorFifo = std::move(other.monitorFifo);
        monitorBuffer = std::move(other.monitorBuffer);
        currentRecordingFile = std::move(other.currentRecordingFile);
        recordWriter = std::move(other.recordWriter);
        wasRecording = other.wasRecording;
        recordStartSample = other.recordStartSample;
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

        activeEffectChain.store(other.activeEffectChain.exchange(nullptr, std::memory_order_acq_rel), std::memory_order_release);
        while (auto opt = other.effectGarbageQueue.pop()) effectGarbageQueue.push(*opt);
        while (auto opt = other.effectVectorGarbageQueue.pop()) effectVectorGarbageQueue.push(*opt);
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
//  RecordingThread — Non-RT thread that drains track FIFOs to disk
// ════════════════════════════════════════════════════════════════════════════
class RecordingThread : public juce::Thread
{
public:
    explicit RecordingThread(MainComponent& mc) : juce::Thread("DAW Recording Thread"), owner(mc) {}
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
    void loadAudioFileIntoTrack(int trackIdx, const juce::File& file);
    void updateDrumRackPatternEditor();
    void loadPluginAsTrackInstrument(int trackIdx, const juce::File& pluginFile);
    
    juce::String activeAutomationParameterId;
    int activeAutomationTrackIdx = -1;
    // In Arrangement mode: the clip the user last clicked (nullptr = none)
    ArrangementClip* selectedArrangementClip = nullptr;

    void prepareToPlay (int samplesPerBlockExpected, double sampleRate) override;
    void getNextAudioBlock (const juce::AudioSourceChannelInfo& bufferToFill) override;
    void releaseResources() override;

    void paint   (juce::Graphics& g) override;
    void resized () override;
    bool keyPressed (const juce::KeyPress& key) override;

    void handleIncomingMidiMessage (juce::MidiInput* source,
                                    const juce::MidiMessage& message) override;
    void timerCallback() override;

    // ── Quit-time save dialog (called by MainWindow) ─────────────────────────
    bool saveIfNeededBeforeQuit();

    // ── Control Surface read-only accessors (5.2) ─────────────────────────────
    // Used by ApcMiniDriver::fullSync() to read current clip grid state.
    // All accessors are message-thread-only (same thread as LED sends).
    ClipState getClipState  (int trackIdx, int sceneIdx) const;
    int       getNumTracks  () const noexcept { return numActiveTracks.load(std::memory_order_relaxed); }
    int       getNumScenes  (int trackIdx) const;

    // ── Control Surface action proxies (5.2) ──────────────────────────────────
    // ApcMiniDriver calls these instead of accessing private sessionView members.
    void csLaunchClip         (int trackIdx, int sceneIdx);
    void csLaunchScene        (int sceneIdx);
    void csPauseClip          (int trackIdx, int sceneIdx);
    void csSelectTrack        (int trackIdx);
    void csTrackArmChanged    (int trackIdx, bool armed);
    void csTrackVolumeChanged (int trackIdx, float gain);
    void csMasterVolumeChanged(float gain);

    bool isProjectDirty() const { return projectIsDirty; }

    void shutdownControlSurfaces() { controlSurfaceManager.removeAllSurfaces(); }

    // ── State exposed to RenderThread and RecordingThread (friends) ───────────────────────────────
    friend class RenderThread;
    friend class RecordingThread;

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
    bool            projectIsDirty { false };
    std::unique_ptr<juce::FileChooser> projectChooser;

    void configureProjectMenu();
    void syncProjectToUI();
    void syncUIToProject();
    void exportProject(const juce::File& outputFile, const juce::String& format);
    void updateWindowTitle();
    void markDirty();

    // ── Plugin Delay Compensation ─────────────────────────────────────────────
    // Scans all track effect chains, computes per-track compensation delay, and
    // atomically writes it into each Track::pdcDelaySamples + pdcLine.
    // Must be called on the message thread after any effect chain change.
    void recalculatePDC();

    // ── Audio Clip Warping (3.1) ──────────────────────────────────────────────
    // Called from loadAudioFileIntoTrack and from project load to initialise the
    // AudioClipPlayer for a warp-enabled arrangement clip.
    void loadAudioClipForTrack(int trackIdx, const juce::File& file);
    // Called whenever the global BPM changes so all warp-enabled clip players
    // get their stretch ratio updated in real time.
    void syncWarpRatiosToTempo();

    // ── Comping (3.2) ─────────────────────────────────────────────────────
    // Called from RecordingThread (via callAsync) after each take is finalised.
    // Appends a new Take to clips[0] on trackIdx, enforces the 8-take cap,
    // and calls loadCompPlayerForTrack.
    void registerNewTake(int trackIdx, const juce::File& file, int64_t startSample);
    // Creates/reloads the CompPlayer for trackIdx from arrangementTracks data.
    void loadCompPlayerForTrack(int trackIdx);
    // Called from the swipe-to-comp gesture (TakeLaneOverlay callback).
    void onCompRegionSwiped(int trackIdx, int clipIdx, int takeIdx,
                            double startBeat, double endBeat);
    // loopWrapCounter: incremented by the render thread on every loop-wrap event.
    // RecordingThread polls this to detect when to start a new take file.
    std::atomic<int> loopWrapCounter { 0 };

    // ── Sidechain Routing (2.1) ───────────────────────────────────────────────
    // Sets the sidechain source for the track at targetTrackIdx.
    // sourceTrackIdx == −1 clears the sidechain.
    void setSidechainSource(int targetTrackIdx, int sourceTrackIdx);

    // ── Group Track Management (2.2) ─────────────────────────────────────────
    void addGroupTrack();
    void deleteGroupTrack(int groupIdx);
    void refreshGroupBadges();  // syncs group badge text/colour on all audio track headers

    // ── Audio Graph Elements ─────────────────────────────────────────────────
    // Tracks are heap-allocated (Track has non-movable std::atomic members).
    // Pre-reserved to 128 so no reallocation occurs during the session
    // (vector pointer stability is required by the lock-free render thread).
    Track masterTrack;
    std::vector<std::unique_ptr<Track>> returnTracks;
    std::atomic<int> numReturnTracks {0};
    std::vector<std::unique_ptr<Track>> audioTracks; // indexed 0..numActiveTracks-1
    std::atomic<int> numActiveTracks {0}; // written UI, read audio+render thread

    // ── Group Tracks (2.2) ────────────────────────────────────────────────────
    // Parallel to returnTracks. Audio tracks route into a group bus instead of
    // the master when their groupBusIndex is >= 0. The group bus then runs its
    // own effect chain and accumulates into the master after all audio tracks.
    std::vector<std::unique_ptr<Track>> groupTracks;
    std::atomic<int> numGroupTracks { 0 };
    std::array<juce::AudioBuffer<float>, 8> groupScratches;
    // UI-thread names for group tracks (parallel to groupTracks[])
    std::vector<juce::String> groupTrackNames;

    // ── Clip Grid State (UI thread only) ─────────────────────────────────────
    // clipGrid[t][s] — each track has its own dynamic scene list
    std::vector<std::vector<ClipData>> clipGrid;
    int selectedTrackIndex = -1;
    int selectedSceneIndex = -1;
    int selectedDrumPadIndex = 0;

    // ── Browser resize state ─────────────────────────────────────────────────
    int browserWidth { 200 };   // persisted; clamped [140, 520]

    // Thin drag-strip sitting on the right edge of the browser panel.
    // Inline nested class so it can call back into MainComponent directly.
    struct BrowserResizer : public juce::Component
    {
        MainComponent& owner;
        int dragStartX   { 0 };
        int dragStartW   { 0 };

        explicit BrowserResizer (MainComponent& o) : owner (o)
        {
            setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
        }

        void paint (juce::Graphics& g) override
        {
            // Subtle handle: a pair of short vertical dashes centred on the strip
            auto b = getLocalBounds().toFloat();
            float cx = b.getCentreX();
            float cy = b.getCentreY();
            g.setColour (juce::Colour (0xff404060));
            for (int i = -1; i <= 1; ++i)
                g.fillRoundedRectangle (cx - 1.0f, cy + i * 5.0f - 5.0f, 2.0f, 10.0f, 1.0f);
        }

        void mouseEnter (const juce::MouseEvent&) override { repaint(); }
        void mouseExit  (const juce::MouseEvent&) override { repaint(); }

        void mouseDown (const juce::MouseEvent& e) override
        {
            dragStartX = e.getScreenX();
            dragStartW = owner.browserWidth;
        }

        void mouseDrag (const juce::MouseEvent& e) override
        {
            int delta = e.getScreenX() - dragStartX;
            owner.browserWidth = juce::jlimit (140, 520, dragStartW + delta);
            owner.resized();
        }
    };

    BrowserResizer  browserResizer { *this };

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
    std::atomic<bool>    renderIsArrangementMode {false};
    std::atomic<int64_t> renderTransportOffset {0};

    // Pending seek: when >= 0 the render thread will consume this once and
    // re-anchor itself.  Written on the message thread, read on render thread.
    std::atomic<int64_t> pendingSeekSample {-1};
    
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

    // ── Control Surface API (5.1) ─────────────────────────────────────────────
    // Owns all registered hardware drivers (ApcMiniDriver, etc.).
    // Processes incoming MIDI before standard note routing, and receives
    // synchronous state notifications to update hardware LEDs via observers.
    ControlSurfaceManager controlSurfaceManager;
    // ── MPE (4.1) ─────────────────────────────────────────────────────────────
    // mpeZone: message-thread-only zone detector + per-channel note tracker.
    // mpeTargetTrack: which audio track receives live MPE expression events.
    //   Updated whenever the user arms/selects a track.
    //   Written on the message thread; read inside handleIncomingMidiMessage.
    MpeZoneManager      mpeZone;
    std::atomic<int>    mpeTargetTrack { 0 };

    // ── Application-Level Pre-Render Buffer ─────────────────────────────────
    // Decouples DSP work from the hard RT deadline of the hardware callback.
    // The render thread fills this asynchronously; getNextAudioBlock only reads.
    AppAudioBuffer appBuffer;
    RenderThread   renderThread;
    RecordingThread recordingThread;

    // Scratch buffer used by the render thread (pre-allocated in prepareToPlay)
    juce::AudioBuffer<float> renderScratch;
    std::array<juce::AudioBuffer<float>, 8> returnScratches;

    // Signal from audio thread → render thread: wake up and fill the buffer
    juce::WaitableEvent renderWakeEvent { true }; // manual-reset

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
