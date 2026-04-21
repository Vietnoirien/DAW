#include "MainComponent.h"

// ════════════════════════════════════════════════════════════════════════════
//  RenderThread::run()
//
//  This is the NON-RT workhorse thread. It does all the heavy DSP:
//    - Drains incoming plugin swap queue
//    - Runs SimplerProcessor per track
//    - Runs Plugin processBlock
//    - Computes RMS
//    - Writes the result into the AppAudioBuffer ring
//
//  The hardware audio callback (getNextAudioBlock) becomes a pure MEMCPY.
//  This is what breaks the stutter cycle on USB interfaces.
// ════════════════════════════════════════════════════════════════════════════

void RenderThread::run()
{
    // Attempt elevated priority for the render thread.
    // On Linux this requires rtprio permission in /etc/security/limits.d/.
    // If permission is denied, JUCE degrades gracefully — no crash.
    owner.renderThread.setPriority(juce::Thread::Priority::highest);

    while (!threadShouldExit())
    {
        // Wait until the audio callback signals us to fill more data.
        // Timeout of 5ms prevents deadlock if signal is missed.
        owner.renderWakeEvent.wait(5);
        owner.renderWakeEvent.reset();

        if (threadShouldExit()) break;

        // Fill as many blocks as the ring buffer has room for.
        while (owner.appBuffer.hasRoomToWrite() && !threadShouldExit())
        {
            const int bs       = owner.appBuffer.getBlockSize();
            const int nCh      = owner.appBuffer.getNumChannels();

            // Retrieve DSP parameters from atomics (safe across threads)
            const int nTracks  = owner.numActiveTracks.load(std::memory_order_relaxed);
            const float mGain  = owner.masterTrack.gain.load(std::memory_order_relaxed);
            const float aGain  = (nTracks > 0)
                                     ? owner.audioTracks[0].gain.load(std::memory_order_relaxed)
                                     : 1.0f;

            // Use the pre-allocated scratch buffer
            owner.renderScratch.clear();

            // ── 1. Lock-free plugin hot-swap ─────────────────────────────
            while (auto optPlugin = owner.pluginLoadQueue.pop())
            {
                if (owner.activePlugin != nullptr)
                    owner.pluginGarbageQueue.push(owner.activePlugin);
                owner.activePlugin = *optPlugin;
            }

            // ── 2. Compute block position ─────────────────────────────────
            // We use renderBlockPosition — not transportClock — because inside
            // the inner while() loop we may render several consecutive blocks
            // per wakeup. renderBlockPosition is incremented by bs at the end
            // of each inner iteration (step 8), so block 2 correctly sees
            // renderPos = T+bs, block 3 sees T+2·bs, etc.  Using
            // transportClock.getPlayheadPosition() instead would return the
            // same stale value for all inner iterations, causing every block
            // after the first to schedule events at the wrong position.
            //
            // renderBlockPosition and transportClock stay in lockstep in
            // steady state (both increment by bs per audio callback) because
            // the prepareToPlay pre-fill writes silence directly into the FIFO
            // without touching renderBlockPosition, so there is no divergence.
            const int64_t blockStart = owner.appBuffer.renderBlockPosition.load(
                std::memory_order_acquire);

            // ── 3. Process pattern sequencing (MIDI events → midiBuffer) ──
            for (int t = 0; t < nTracks; ++t)
                owner.audioTracks[t].processPatternCommands(blockStart, bs, owner.transportClock);

            // ── 4. Per-track sampler playback ─────────────────────────────
            for (int t = 0; t < nTracks; ++t)
                owner.samplerDSPs[t].processBlock(owner.renderScratch,
                                                  owner.audioTracks[t].midiBuffer);

            // ── 5. Apply track gain ───────────────────────────────────────
            for (int ch = 0; ch < nCh; ++ch)
            {
                auto* p = owner.renderScratch.getWritePointer(ch);
                for (int i = 0; i < bs; ++i)
                    p[i] *= aGain;
            }

            // ── 6. Plugin processBlock (NO LOCK) ─────────────────────────
            if (owner.activePlugin != nullptr)
            {
                // Collect hardware MIDI for this synthetic block
                juce::MidiBuffer incomingMidi;
                owner.midiCollector.removeNextBlockOfMessages(incomingMidi, bs);

                if (nTracks > 0)
                    incomingMidi.addEvents(owner.audioTracks[0].midiBuffer, 0, bs, 0);

                owner.activePlugin->processBlock(owner.renderScratch, incomingMidi);
            }

            // ── 7. Apply master fader & accumulate RMS ────────────────────
            float sumRmsMaster = 0.0f;
            float sumRmsAudio  = 0.0f;
            for (int ch = 0; ch < nCh; ++ch)
            {
                auto* p = owner.renderScratch.getWritePointer(ch);
                for (int i = 0; i < bs; ++i)
                {
                    p[i] *= mGain;
                    const float s = p[i];
                    sumRmsMaster += s * s;
                    if (ch == 0) sumRmsAudio += s * s;
                }
            }

            owner.masterTrack.rmsLevel.store(
                std::sqrt(sumRmsMaster / (bs * nCh)), std::memory_order_relaxed);
            if (nTracks > 0)
                owner.audioTracks[0].rmsLevel.store(
                    std::sqrt(sumRmsAudio / bs), std::memory_order_relaxed);

            // ── 8. Advance render position counter ────────────────────────
            owner.appBuffer.renderBlockPosition.fetch_add(bs, std::memory_order_release);

            // ── 9. Push rendered block into the ring ──────────────────────
            owner.appBuffer.writeBlock(owner.renderScratch);
        }
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  MainComponent
// ════════════════════════════════════════════════════════════════════════════

MainComponent::MainComponent()
    : renderThread(*this)
{
    // ── Application Workspace & Settings ───────────────────────────────────────
    initialiseWorkspace();


    formatManager.registerBasicFormats();
    pluginFormatManager.addDefaultFormats();

    topBar = std::make_unique<TopBarComponent>(transportClock, deviceManager);
    addAndMakeVisible(topBar.get());
    addAndMakeVisible(browser);
    addAndMakeVisible(sessionView);
    addAndMakeVisible(deviceView);
    addAndMakeVisible(patternEditor);

    configureProjectMenu();

    browser.onFolderSelected = [this](const juce::File& f) {
        if (userSettings != nullptr) {
            userSettings->setValue("browserDirectory", f.getFullPathName());
            userSettings->saveIfNeeded();
        }
    };

    // ── File Drop → Background Decode → Lock-Free Buffer Handoff ─────────────
    deviceView.onFileDropped = [this](const juce::File& file) {
        const int trackIdx = selectedTrackIndex;
        if (trackIdx < 0 || trackIdx >= MAX_TRACKS) {
            DBG("onFileDropped: no track selected — ignoring.");
            return;
        }
        loadAudioFileIntoTrack(trackIdx, file);
    };

    // ── Transport ─────────────────────────────────────────────────────────────
    topBar->playBtn.onClick = [this] { transportClock.play(); };

    topBar->stopBtn.onClick = [this] {
        transportClock.stop();
        int nTracks = numActiveTracks.load(std::memory_order_relaxed);
        for (int t = 0; t < nTracks; ++t)
            audioTracks[t].commandQueue.push({ TrackCommand::Type::StopPattern, nullptr, 0 });
        for (int t = 0; t < nTracks; ++t)
            for (int s = 0; s < NUM_SCENES; ++s)
                if (clipGrid[t][s].isPlaying) {
                    clipGrid[t][s].isPlaying = false;
                    sessionView.setClipData(t, s, clipGrid[t][s]);
                }
        sessionView.setSceneActive(-1, false);
    };

    // ── Plugin Loading ────────────────────────────────────────────────────────
    topBar->loadPluginBtn.onClick = [this] {
        myChooser = std::make_unique<juce::FileChooser>("Select a VST3 or LV2 Plugin...",
            juce::File::getSpecialLocation(juce::File::userHomeDirectory), "*.vst3;*.lv2");
        myChooser->launchAsync(
            juce::FileBrowserComponent::openMode |
            juce::FileBrowserComponent::canSelectFiles |
            juce::FileBrowserComponent::canSelectDirectories,
            [this](const juce::FileChooser& fc) {
                auto file = fc.getResult();
                if (file == juce::File{}) return;

                juce::String errorMessage;
                for (auto* format : pluginFormatManager.getFormats()) {
                    if (!format->fileMightContainThisPluginType(file.getFullPathName())) continue;
                    juce::OwnedArray<juce::PluginDescription> foundPlugins;
                    format->findAllTypesForFile(foundPlugins, file.getFullPathName());
                    if (foundPlugins.isEmpty()) continue;

                    auto instance = format->createInstanceFromDescription(
                        *foundPlugins[0], currentSampleRate, currentBufferSize, errorMessage);
                    if (instance == nullptr) {
                        juce::Logger::writeToLog("Plugin load failed: " + errorMessage);
                        continue;
                    }

                    instance->prepareToPlay(currentSampleRate, currentBufferSize);
                    pluginWindow = nullptr;

                    if (instance->hasEditor()) {
                        pluginWindow = std::make_unique<juce::DocumentWindow>(
                            "Plugin Editor", juce::Colours::lightgrey,
                            juce::DocumentWindow::closeButton);
                        if (auto* editor = instance->createEditorIfNeeded()) {
                            pluginWindow->setContentOwned(editor, true);
                            pluginWindow->setResizable(true, false);
                            pluginWindow->centreWithSize(editor->getWidth(), editor->getHeight());
                            pluginWindow->setVisible(true);
                        }
                    }
                    pluginLoadQueue.push(instance.release());
                    break;
                }
            });
    };

    // ── Pattern Editor ────────────────────────────────────────────────────────
    patternEditor.onEuclideanChanged = [this](int k, int n) {
        if (!transportClock.getIsPlaying() || selectedTrackIndex < 0) return;
        auto* p = euclideanPool.rentPattern();
        p->generate(k, n);
        audioTracks[selectedTrackIndex].commandQueue.push(
            { TrackCommand::Type::PlayPattern, p, transportClock.getNextBarPosition() });
        if (selectedSceneIndex >= 0) {
            auto& clip = clipGrid[selectedTrackIndex][selectedSceneIndex];
            clip.euclideanSteps  = n;
            clip.euclideanPulses = k;
            // Mirror the generated map into clipGrid so that scene launch and
            // project save always have a single consistent source of truth.
            // Without this, relaunching a scene after slider changes would use
            // a stale hitMap from a previous manual circle edit.
            clip.hitMap.assign(p->getHitMap().begin(), p->getHitMap().end());
        }
    };

    patternEditor.onEuclideanHitMapChanged = [this](const std::vector<uint8_t>& map) {
        if (!transportClock.getIsPlaying() || selectedTrackIndex < 0) return;
        auto* p = euclideanPool.rentPattern();
        p->setHitMap(map);
        audioTracks[selectedTrackIndex].commandQueue.push(
            { TrackCommand::Type::PlayPattern, p, transportClock.getNextBarPosition() });
        if (selectedSceneIndex >= 0)
            clipGrid[selectedTrackIndex][selectedSceneIndex].hitMap = map;
    };

    // ── Session View Callbacks ────────────────────────────────────────────────
    sessionView.onCreateClip = [this](int t, int s) {
        if (t < 0 || t >= numActiveTracks.load(std::memory_order_relaxed)) return;
        auto& clip           = clipGrid[t][s];
        clip.hasClip         = true;
        clip.name            = "Pattern " + juce::String(s + 1);
        clip.colour          = juce::Colour::fromHSV(
            std::fmod((float)(t*47 + s*31) / 360.0f, 1.0f), 0.65f, 0.70f, 1.0f);
        clip.euclideanSteps  = 16;
        clip.euclideanPulses = 4;
        clip.hitMap.clear();
        sessionView.setClipData(t, s, clip);
        selectedTrackIndex = t;
        selectedSceneIndex = s;
        sessionView.setClipSelected(t, s);
        sessionView.setTrackSelected(t);
        patternEditor.loadClipData(clip);
        resized();
    };

    sessionView.onSelectClip = [this](int t, int s) {
        selectedTrackIndex = t;
        selectedSceneIndex = s;
        sessionView.setClipSelected(t, s);
        sessionView.setTrackSelected(t);
        patternEditor.loadClipData(clipGrid[t][s]);
        resized();
        if (t >= 0 && t < MAX_TRACKS)
            deviceView.showFile(loadedFiles[t]);
        else
            deviceView.clear();
    };

    sessionView.onLaunchClip = [this](int t, int s) {
        sessionView.onSelectClip(t, s);

        if (!transportClock.getIsPlaying()) {
            transportClock.stop(); // to ensure playhead zero
            transportClock.play();
        }

        auto& clip = clipGrid[t][s];
        auto* p    = euclideanPool.rentPattern();
        if (!clip.hitMap.empty()) p->setHitMap(clip.hitMap);
        else                      p->generate(clip.euclideanPulses, clip.euclideanSteps);
        audioTracks[t].commandQueue.push(
            { TrackCommand::Type::PlayPattern, p, transportClock.getNextBarPosition() });
        clip.isPlaying = true;
        sessionView.setClipData(t, s, clip);
    };

    sessionView.onSelectTrack = [this](int t) {
        selectedTrackIndex = t;
        selectedSceneIndex = -1;
        sessionView.setClipSelected(t, -1);
        sessionView.setTrackSelected(t);
        resized();
        if (t >= 0 && t < MAX_TRACKS)
            deviceView.showFile(loadedFiles[t]);
        else
            deviceView.clear();
    };

    sessionView.onDeleteClip = [this](int t, int s) {
        if (t < 0 || t >= MAX_TRACKS) return;
        auto& clip = clipGrid[t][s];
        if (!clip.hasClip) return;

        if (clip.isPlaying) {
            audioTracks[t].commandQueue.push({ TrackCommand::Type::StopPattern, nullptr, 0 });
        }
        clip = ClipData();
        sessionView.setClipData(t, s, clip);
        if (selectedTrackIndex == t && selectedSceneIndex == s) {
            patternEditor.setVisible(false);
            selectedSceneIndex = -1;
            resized();
        }
    };

    sessionView.onDeleteTrack = [this](int trackIndex) {
        int nTracks = numActiveTracks.load(std::memory_order_acquire);
        if (trackIndex < 0 || trackIndex >= nTracks) return;

        bool wasRunning = renderThread.isThreadRunning();
        if (wasRunning) renderThread.stopThread(500);

        for (int t = trackIndex; t < nTracks - 1; ++t) {
            audioTracks[t].moveFrom(audioTracks[t + 1]);
            samplerDSPs[t].moveFrom(samplerDSPs[t + 1]);
            loadedFiles[t] = loadedFiles[t + 1];
            for (int s = 0; s < NUM_SCENES; ++s) {
                clipGrid[t][s] = clipGrid[t + 1][s];
            }
        }

        int last = nTracks - 1;
        audioTracks[last].clear();
        samplerDSPs[last].clear();
        loadedFiles[last] = juce::File();
        for (int s = 0; s < NUM_SCENES; ++s) {
            clipGrid[last][s] = ClipData();
        }

        numActiveTracks.fetch_sub(1, std::memory_order_release);
        sessionView.removeTrack(trackIndex);

        if (selectedTrackIndex == trackIndex) {
            selectedTrackIndex = -1;
            selectedSceneIndex = -1;
            patternEditor.setVisible(false);
            deviceView.onFileDropped(juce::File()); // Clear device view if needed
        } else if (selectedTrackIndex > trackIndex) {
            selectedTrackIndex--;
        }
        
        resized();
        if (wasRunning) renderThread.startThread();
    };

    sessionView.onLaunchScene = [this](int sceneIdx) {
        if (!transportClock.getIsPlaying()) {
            transportClock.stop(); // zero playhead
            transportClock.play();
        }
        double schedSample = transportClock.getNextBarPosition();
        int nTracks = numActiveTracks.load(std::memory_order_relaxed);
        for (int t = 0; t < nTracks; ++t) {
            if (clipGrid[t][sceneIdx].hasClip) {
                auto& clip = clipGrid[t][sceneIdx];
                auto* p    = euclideanPool.rentPattern();
                if (!clip.hitMap.empty()) p->setHitMap(clip.hitMap);
                else                      p->generate(clip.euclideanPulses, clip.euclideanSteps);
                audioTracks[t].commandQueue.push({ TrackCommand::Type::PlayPattern, p, schedSample });
                clip.isPlaying = true;
                sessionView.setClipData(t, sceneIdx, clip);
            } else {
                audioTracks[t].commandQueue.push({ TrackCommand::Type::StopPattern, nullptr, schedSample });
            }
        }
        sessionView.setSceneActive(sceneIdx, true);
    };

    sessionView.onInstrumentDropped = [this](int trackIdx, const juce::String& /*type*/) {
        int nTracks = numActiveTracks.load(std::memory_order_relaxed);
        if (trackIdx >= 0 && trackIdx < nTracks) {
            sessionView.gridContent.columns[trackIdx]->header.hasInstrument = true;
            sessionView.gridContent.columns[trackIdx]->header.repaint();
        } else {
            if (nTracks >= MAX_TRACKS) return;
            int newIdx = numActiveTracks.fetch_add(1, std::memory_order_relaxed);
            sessionView.addTrack(TrackType::Audio, "Track " + juce::String(newIdx + 1));
            sessionView.gridContent.columns[newIdx]->header.hasInstrument = true;
            sessionView.gridContent.columns[newIdx]->header.repaint();
        }
    };

    // ── Audio Device ──────────────────────────────────────────────────
    // Load device settings is deferred to finishInitialisation() after Workspace is selected.

    // Save device settings whenever the dialog is closed.
    topBar->onSettingsClosed = [this] { saveDeviceSettings(); };

    auto midiInputs = juce::MidiInput::getAvailableDevices();
    for (const auto& dev : midiInputs)
        deviceManager.setMidiInputDeviceEnabled(dev.identifier, true);
    deviceManager.addMidiInputDeviceCallback("", this);

    setSize(1200, 800);
    startTimerHz(30);
}

MainComponent::~MainComponent()
{
    stopTimer();
    deviceManager.removeMidiInputDeviceCallback("", this);

    // 1. Destroy plugin editor (no more references to plugin instance).
    pluginWindow = nullptr;

    // 2. Stop the render thread before shutting down the audio device.
    renderThread.stopThread(500);

    // 3. Stop audio device (blocks until last getNextAudioBlock returns).
    shutdownAudio();

    // 4. Safe cleanup — both threads are stopped.
    delete activePlugin;
    activePlugin = nullptr;
    while (auto opt = pluginLoadQueue.pop())    delete *opt;
    while (auto opt = pluginGarbageQueue.pop()) delete *opt;
}

void MainComponent::prepareToPlay(int samplesPerBlockExpected, double sampleRate)
{
    currentSampleRate = sampleRate;
    currentBufferSize = samplesPerBlockExpected;
    transportClock.setSampleRate(currentSampleRate);
    midiCollector.reset(sampleRate);

    // Allocate the application ring buffer (8 blocks of headroom).
    appBuffer.prepare(2, samplesPerBlockExpected, sampleRate);

    // Pre-allocate the render scratch buffer (2ch × blockSize).
    renderScratch.setSize(2, samplesPerBlockExpected);
    renderScratch.clear();

    // Pre-fill the ring with silence so the first callback never underruns.
    juce::AudioBuffer<float> silence(2, samplesPerBlockExpected);
    silence.clear();
    for (int i = 0; i < AppAudioBuffer::RING_CAPACITY_BLOCKS - 1; ++i)
        appBuffer.writeBlock(silence);

    // Start the render thread (it will wait on renderWakeEvent initially).
    if (!renderThread.isThreadRunning())
        renderThread.startThread();
}

// ════════════════════════════════════════════════════════════════════════════
//  getNextAudioBlock — RT audio callback.
//
//  This function is now TRIVIALLY SIMPLE:
//    1. Advance the transport clock (cheap atomic add)
//    2. Signal the render thread to fill more data
//    3. Copy pre-rendered audio from the ring buffer into the hardware buffer
//
//  NO DSP. NO LOCKS. NO ALLOCATIONS. Pure copy.
// ════════════════════════════════════════════════════════════════════════════
void MainComponent::getNextAudioBlock(const juce::AudioSourceChannelInfo& bufferToFill)
{
    const int numSamples = bufferToFill.numSamples;

    // Advance the global transport clock (atomic — safe here).
    transportClock.advanceBy(numSamples);

    // Signal the render thread to fill more data into the ring.
    // WaitableEvent::signal() is lock-free and safe from the audio thread.
    renderWakeEvent.signal();

    // Build a small destination view and copy from the ring.
    juce::AudioBuffer<float> dest(
        bufferToFill.buffer->getArrayOfWritePointers(),
        bufferToFill.buffer->getNumChannels(),
        bufferToFill.startSample,
        numSamples);

    appBuffer.readBlock(dest, numSamples);
}

void MainComponent::releaseResources()
{
    renderThread.stopThread(200);
    appBuffer.releaseResources();
    if (activePlugin != nullptr)
        activePlugin->releaseResources();
}

void MainComponent::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colours::black);
}

void MainComponent::resized()
{
    auto bounds = getLocalBounds();
    topBar->setBounds(bounds.removeFromTop(40));
    browser.setBounds(bounds.removeFromLeft(200));

    auto bottomPanel = bounds.removeFromBottom(250);
    
    if (selectedSceneIndex >= 0 && selectedTrackIndex >= 0 && clipGrid[selectedTrackIndex][selectedSceneIndex].hasClip)
    {
        deviceView.setBounds(bottomPanel.removeFromLeft(bottomPanel.getWidth() / 2));
        patternEditor.setBounds(bottomPanel);
        patternEditor.setVisible(true);
    }
    else
    {
        deviceView.setBounds(bottomPanel);
        patternEditor.setVisible(false);
    }

    sessionView.setBounds(bounds);
}

void MainComponent::timerCallback()
{
    auto applyDecay = [](float& cur, float target) {
        cur = (target > cur) ? target : cur * 0.8f + target * 0.2f;
    };

    const int nTracks = numActiveTracks.load(std::memory_order_relaxed);

    if (nTracks > 0)
        applyDecay(audioLevelDisplay,  audioTracks[0].rmsLevel.load(std::memory_order_relaxed));
    applyDecay(returnLevelDisplay, returnTrackA.rmsLevel.load(std::memory_order_relaxed));
    applyDecay(masterLevelDisplay, masterTrack.rmsLevel.load(std::memory_order_relaxed));

    // ── GC: Pattern pool ──────────────────────────────────────────────────────
    auto gcTrack = [&](Track& t) {
        while (auto oldPtr = t.garbageQueue.pop()) {
            Pattern* p = *oldPtr;
            if (auto* ep = dynamic_cast<EuclideanPattern*>(p)) euclideanPool.returnPattern(ep);
            else if (auto* mp = dynamic_cast<MidiPattern*>(p))   midiPool.returnPattern(mp);
        }
    };
    gcTrack(masterTrack);
    gcTrack(returnTrackA);
    for (int t = 0; t < nTracks; ++t) gcTrack(audioTracks[t]);

    // ── GC: Sampler buffers ───────────────────────────────────────────────────
    for (int t = 0; t < nTracks; ++t)
        while (auto* old = samplerDSPs[t].popGarbage())
            delete old;

    // ── GC: Old plugin instances ──────────────────────────────────────────────
    while (auto opt = pluginGarbageQueue.pop())
        delete *opt;

    // ── Update Playhead Phase for UI ──────────────────────────────────────────
    int64_t playhead = transportClock.getPlayheadPosition();
    double spb = transportClock.getSamplesPerBeat();
    bool playing = transportClock.getIsPlaying();

    for (int t = 0; t < nTracks; ++t) {
        float phase = -1.0f;
        if (playing && audioTracks[t].currentPattern && spb > 0.0) {
            double lenBeats = audioTracks[t].currentPattern->getLengthBeats();
            double lenSamples = lenBeats * spb;
            if (lenSamples > 0.0) {
                double elapsed = static_cast<double>(playhead - audioTracks[t].patternStartSample);
                if (elapsed >= 0.0) {
                    phase = static_cast<float>(std::fmod(elapsed, lenSamples) / lenSamples);
                }
            }
        }
        
        sessionView.setTrackPlayhead(t, phase);
        if (t == selectedTrackIndex) {
            patternEditor.setPlayheadPhase(phase);
        }
    }

    // ── Diagnostics: underrun counter ─────────────────────────────────────────
    int underruns = appBuffer.getAndResetUnderrunCount();
    if (underruns > 0)
        DBG("AppAudioBuffer: " << underruns << " underrun(s) this interval");

    // ── Targeted repaint: only top bar (shows transport clock) ────────────────
    if (topBar != nullptr)
        topBar->repaint();
}

void MainComponent::handleIncomingMidiMessage(juce::MidiInput* source,
                                               const juce::MidiMessage& message)
{
    juce::ignoreUnused(source);
    midiCollector.addMessageToQueue(message);
}

// ════════════════════════════════════════════════════════════════════════════
//  Settings Persistence
// ════════════════════════════════════════════════════════════════════════════

void MainComponent::saveDeviceSettings()
{
    if (userSettings != nullptr)
    {
        if (auto xml = deviceManager.createStateXml())
            userSettings->setValue("audioDeviceState", xml.get());

        userSettings->saveIfNeeded();
        DBG("Device settings saved.");
    }
}

void MainComponent::loadDeviceSettings()
{
    std::unique_ptr<juce::XmlElement> savedState;
    if (userSettings != nullptr) {
        savedState = userSettings->getXmlValue("audioDeviceState");
        
        juce::String savedDir = userSettings->getValue("browserDirectory");
        if (savedDir.isNotEmpty()) {
            juce::File f(savedDir);
            if (f.isDirectory())
                browser.setDirectory(f);
        }
    }

    setAudioChannels(2, 2, savedState.get());
    DBG("Device settings loaded successfully.");
}

// ════════════════════════════════════════════════════════════════════════════
//  Project Management & Workspace Lifecycle
// ════════════════════════════════════════════════════════════════════════════

bool MainComponent::ensureWorkspaceDirectory()
{
    juce::File appDataDir = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("LinuxDAW");
    if (!appDataDir.exists()) appDataDir.createDirectory();
    
    juce::File wfFile = appDataDir.getChildFile("workspace_pointer.txt");
    if (wfFile.existsAsFile())
    {
        juce::String path = wfFile.loadFileAsString().trim();
        juce::File wf(path);
        if (wf.isDirectory()) {
            workspaceDirectory = wf;
            return true;
        }
    }
    return false;
}

void MainComponent::initialiseWorkspace()
{
    if (!ensureWorkspaceDirectory())
    {
        projectChooser = std::make_unique<juce::FileChooser>("Select main folder for user projects (Workspace)",
                                  juce::File::getSpecialLocation(juce::File::userHomeDirectory));
        
        projectChooser->launchAsync(juce::FileBrowserComponent::canSelectDirectories | juce::FileBrowserComponent::openMode,
            [this](const juce::FileChooser& chooser) {
                auto result = chooser.getResult();
                if (result != juce::File{}) {
                    workspaceDirectory = result;
                } else {
                    workspaceDirectory = juce::File::getSpecialLocation(juce::File::userHomeDirectory).getChildFile("LinuxDAW_Projects");
                }
                
                workspaceDirectory.createDirectory();
                juce::File appDataDir = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("LinuxDAW");
                appDataDir.createDirectory();
                appDataDir.getChildFile("workspace_pointer.txt").replaceWithText(workspaceDirectory.getFullPathName());
                
                finishInitialisation();
            });
    }
    else
    {
        finishInitialisation();
    }
}

void MainComponent::finishInitialisation()
{
    workspaceDirectory.getChildFile("Settings").createDirectory();
    workspaceDirectory.getChildFile("Projects").createDirectory();
    workspaceDirectory.getChildFile("Samples").createDirectory();

    juce::File settingsFile = workspaceDirectory.getChildFile("Settings").getChildFile("LinuxDAW.settings");
    
    juce::PropertiesFile::Options opts;
    opts.applicationName = "LinuxDAW";
    opts.filenameSuffix  = "settings";
    userSettings = std::make_unique<juce::PropertiesFile>(settingsFile, opts);

    if (juce::RuntimePermissions::isRequired(juce::RuntimePermissions::recordAudio) &&
        !juce::RuntimePermissions::isGranted(juce::RuntimePermissions::recordAudio)) {
        juce::RuntimePermissions::request(juce::RuntimePermissions::recordAudio,
            [&](bool granted) { if (granted) loadDeviceSettings(); });
    } else {
        loadDeviceSettings();
    }
}

void MainComponent::configureProjectMenu()
{
    topBar->onNewProject = [this] {
        transportClock.stop();
        projectManager.createNewProject();
        currentProjectFile = juce::File();
        syncUIToProject();
    };

    topBar->onOpenProject = [this] {
        projectChooser = std::make_unique<juce::FileChooser>("Open Project...", workspaceDirectory.getChildFile("Projects"), "*.vtn");
        projectChooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
            [this](const juce::FileChooser& fc) {
                auto result = fc.getResult();
                if (result != juce::File{}) {
                    transportClock.stop();
                    if (projectManager.loadProject(result)) {
                        currentProjectFile = result;
                        syncUIToProject();
                    } else {
                        juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon, "Error", "Could not load project file.");
                    }
                }
            });
    };

    topBar->onSaveProject = [this] {
        if (currentProjectFile == juce::File{}) {
            topBar->onSaveProjectAs();
        } else {
            syncProjectToUI();
            projectManager.saveProject(currentProjectFile);
        }
    };

    topBar->onSaveProjectAs = [this] {
        projectChooser = std::make_unique<juce::FileChooser>("Save Project As...", workspaceDirectory.getChildFile("Projects"), "*.vtn");
        projectChooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles,
            [this](const juce::FileChooser& fc) {
                auto result = fc.getResult();
                if (result != juce::File{}) {
                    if (!result.hasFileExtension(".vtn")) {
                        result = result.withFileExtension(".vtn");
                    }
                    currentProjectFile = result;
                    syncProjectToUI();
                    projectManager.saveProject(currentProjectFile);
                }
            });
    };
}

void MainComponent::syncUIToProject()
{
    // Clear current tracks and UI
    numActiveTracks.store(0, std::memory_order_relaxed);
    for (int t = 0; t < MAX_TRACKS; t++) {
        loadedFiles[t] = juce::File();
        // Zero all clip grids
        for (int s = 0; s < NUM_SCENES; s++) {
            clipGrid[t][s] = ClipData();
            sessionView.setClipData(t, s, clipGrid[t][s]);
        }
        audioTracks[t].gain.store(1.0f);
        audioTracks[t].commandQueue.push({ TrackCommand::Type::StopPattern, nullptr, 0 });
    }
    
    selectedTrackIndex = -1;
    selectedSceneIndex = -1;
    
    // Clear session view UI columns
    sessionView.gridContent.columns.clear();
    sessionView.resized();

    auto& pTree = projectManager.getTree();
    
    // 1. Restore Transport
    if (pTree.hasProperty("bpm")) {
        double savedBpm = pTree.getProperty("bpm");
        transportClock.setBpm(savedBpm);
        topBar->bpmSlider.setValue(savedBpm, juce::dontSendNotification);
    }
    
    // 2. Restore Tracks
    auto tracksTree = pTree.getChildWithName("Tracks");
    if (tracksTree.isValid())
    {
        for (int i = 0; i < tracksTree.getNumChildren(); ++i)
        {
            auto trackNode = tracksTree.getChild(i);
            if (trackNode.hasType("Track"))
            {
                int tIdx = trackNode.getProperty("index", -1);
                if (tIdx >= 0 && tIdx < MAX_TRACKS)
                {
                    numActiveTracks.store(std::max(numActiveTracks.load(), tIdx + 1), std::memory_order_relaxed);
                    
                    juce::String name = trackNode.getProperty("name", "Track " + juce::String(tIdx + 1));
                    float gain = trackNode.getProperty("gain", 1.0f);
                    audioTracks[tIdx].gain.store(gain);
                    
                    sessionView.addTrack(TrackType::Audio, name);
                    
                    // Sampler file
                    auto samplerNode = trackNode.getChildWithName("Sampler");
                    if (samplerNode.isValid()) {
                        juce::String filePath = samplerNode.getProperty("file_path", "");
                        juce::File f(filePath);
                        if (f.existsAsFile()) {
                            loadAudioFileIntoTrack(tIdx, f);
                            sessionView.gridContent.columns[tIdx]->header.hasInstrument = true;
                        }
                    }

                    // Clips
                    auto clipsNode = trackNode.getChildWithName("Clips");
                    if (clipsNode.isValid()) {
                        for (int c = 0; c < clipsNode.getNumChildren(); ++c) {
                            auto clipNode = clipsNode.getChild(c);
                            if (clipNode.hasType("Clip")) {
                                int sIdx = clipNode.getProperty("scene", -1);
                                if (sIdx >= 0 && sIdx < NUM_SCENES) {
                                    ClipData d;
                                    d.hasClip = clipNode.getProperty("hasClip", false);
                                    d.isPlaying = false;
                                    d.name = clipNode.getProperty("name", "Pattern " + juce::String(sIdx + 1));
                                    d.euclideanSteps = clipNode.getProperty("euclideanSteps", 16);
                                    d.euclideanPulses = clipNode.getProperty("euclideanPulses", 4);
                                    // Restore custom hitMap from hex string if present.
                                    juce::String hex = clipNode.getProperty("hitMap", "");
                                    if (hex.isNotEmpty() && hex.length() % 2 == 0) {
                                        d.hitMap.clear();
                                        for (int ci = 0; ci < hex.length(); ci += 2)
                                            d.hitMap.push_back((uint8_t) hex.substring(ci, ci + 2).getHexValue32());
                                    }
                                    clipGrid[tIdx][sIdx] = d;
                                    sessionView.setClipData(tIdx, sIdx, d);
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

void MainComponent::syncProjectToUI()
{
    auto& pTree = projectManager.getTree();
    pTree.setProperty("bpm", transportClock.getBpm(), nullptr);
    
    auto tracksTree = pTree.getChildWithName("Tracks");
    if (!tracksTree.isValid()) {
        tracksTree = juce::ValueTree("Tracks");
        pTree.addChild(tracksTree, -1, nullptr);
    }
    tracksTree.removeAllChildren(nullptr);
    
    int nTracks = numActiveTracks.load(std::memory_order_relaxed);
    for (int t = 0; t < nTracks; ++t)
    {
        juce::ValueTree trackNode("Track");
        trackNode.setProperty("index", t, nullptr);
        
        juce::String name = "Track " + juce::String(t + 1);
        if (t < sessionView.gridContent.columns.size()) {
            name = sessionView.gridContent.columns[t]->header.trackName;
        }
        trackNode.setProperty("name", name, nullptr);
        trackNode.setProperty("gain", audioTracks[t].gain.load(std::memory_order_relaxed), nullptr);

        if (loadedFiles[t].existsAsFile()) {
            juce::ValueTree samplerNode("Sampler");
            samplerNode.setProperty("file_path", loadedFiles[t].getFullPathName(), nullptr);
            trackNode.addChild(samplerNode, -1, nullptr);
        }

        juce::ValueTree clipsNode("Clips");
        for (int s = 0; s < NUM_SCENES; ++s) {
            auto& clip = clipGrid[t][s];
            if (clip.hasClip) {
                juce::ValueTree clipNode("Clip");
                clipNode.setProperty("scene", s, nullptr);
                clipNode.setProperty("hasClip", true, nullptr);
                clipNode.setProperty("name", clip.name, nullptr);
                clipNode.setProperty("euclideanSteps", clip.euclideanSteps, nullptr);
                clipNode.setProperty("euclideanPulses", clip.euclideanPulses, nullptr);
                // Serialize hitMap as a hex string so custom patterns survive save/load.
                if (!clip.hitMap.empty()) {
                    juce::String hex;
                    for (uint8_t b : clip.hitMap)
                        hex += juce::String::toHexString(b).paddedLeft('0', 2);
                    clipNode.setProperty("hitMap", hex, nullptr);
                }
                clipsNode.addChild(clipNode, -1, nullptr);
            }
        }
        if (clipsNode.getNumChildren() > 0)
            trackNode.addChild(clipsNode, -1, nullptr);
            
        tracksTree.addChild(trackNode, -1, nullptr);
    }
}

void MainComponent::loadAudioFileIntoTrack(int trackIdx, const juce::File& file)
{
    if (trackIdx < 0 || trackIdx >= MAX_TRACKS) return;

    loadedFiles[trackIdx] = file;

    juce::Thread::launch([this, file, trackIdx] {
        if (auto* reader = formatManager.createReaderFor(file)) {
            auto* newBuffer = new juce::AudioBuffer<float>(
                reader->numChannels, (int)reader->lengthInSamples);
            reader->read(newBuffer, 0, (int)reader->lengthInSamples, 0, true, true);
            delete reader;

            juce::MessageManager::callAsync([this, newBuffer, trackIdx, file] {
                samplerDSPs[trackIdx].loadNewBuffer(newBuffer);
                if (trackIdx == selectedTrackIndex) {
                    deviceView.showFile(file);
                }
            });
        }
    });
}
