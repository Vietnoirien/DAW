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
            // renderPos = T+bs, block 3 sees T+2*bs, etc.
            const int64_t blockStart = owner.appBuffer.renderBlockPosition.load(
                std::memory_order_acquire);

            // ── 3. Process pattern sequencing (MIDI events -> midiBuffer) ──
            for (int t = 0; t < nTracks; ++t)
                owner.audioTracks[t].processPatternCommands(blockStart, bs, owner.transportClock);

            // ── 4. Per-track render + gain + sum into master mix ───────────
            // renderScratch is the MASTER MIX accumulator.
            // trackScratch is a temporary per-track render buffer.
            owner.renderScratch.clear();

            // Pre-pass: is any track soloed? (determines whether solo silences others)
            bool anySoloed = false;
            for (int t = 0; t < nTracks; ++t)
                if (owner.audioTracks[t].soloed.load(std::memory_order_relaxed))
                    { anySoloed = true; break; }

            juce::AudioBuffer<float> trackScratch (nCh, bs);
            juce::AudioBuffer<float> returnScratch (nCh, bs);
            returnScratch.clear();

            for (int t = 0; t < nTracks; ++t)
            {
                const bool muted  = owner.audioTracks[t].muted.load(std::memory_order_relaxed);
                const bool soloed = owner.audioTracks[t].soloed.load(std::memory_order_relaxed);
                // A track is audible if: not muted AND (nothing is soloed OR this track is soloed)
                const bool audible = !muted && (!anySoloed || soloed);
                const float tGain = audible
                    ? owner.audioTracks[t].gain.load(std::memory_order_relaxed)
                    : 0.0f;

                trackScratch.clear();
                if (auto* inst = owner.audioTracks[t].activeInstrument.load(std::memory_order_acquire)) {
                    inst->processBlock(trackScratch, owner.audioTracks[t].midiBuffer);
                }

                for (int e = 0; e < Track::MAX_EFFECTS; ++e) {
                    if (auto* effect = owner.audioTracks[t].effectChain[e].load(std::memory_order_acquire)) {
                        effect->processBlock(trackScratch);
                    }
                }

                const float tSendALevel = audible ? owner.audioTracks[t].sendALevel.load(std::memory_order_relaxed) : 0.0f;

                // Accumulate track (with effective gain) into master mix and send to return A (post-fader)
                if (tGain > 0.0f || tSendALevel > 0.0f)
                {
                    for (int ch = 0; ch < nCh; ++ch)
                    {
                        auto* dst = owner.renderScratch.getWritePointer(ch);
                        auto* retDst = returnScratch.getWritePointer(ch);
                        const auto* src = trackScratch.getReadPointer(ch);
                        for (int i = 0; i < bs; ++i) {
                            dst[i] += src[i] * tGain;
                            retDst[i] += src[i] * tGain * tSendALevel;
                        }
                    }
                }

                // Per-track RMS reflects effective (post-mute/solo) level
                float sumRmsTrack = 0.0f;
                const auto* src0 = trackScratch.getReadPointer(0);
                for (int i = 0; i < bs; ++i) { float s = src0[i] * tGain; sumRmsTrack += s * s; }
                owner.audioTracks[t].rmsLevel.store(
                    std::sqrt(sumRmsTrack / bs), std::memory_order_relaxed);
            }

            // ── Process Return Track Effects & Accumulate to Master ───────
            for (int e = 0; e < Track::MAX_EFFECTS; ++e) {
                if (auto* effect = owner.returnTrackA.effectChain[e].load(std::memory_order_acquire)) {
                    effect->processBlock(returnScratch);
                }
            }

            const float retGain = owner.returnTrackA.gain.load(std::memory_order_relaxed);
            float sumRmsReturn = 0.0f;
            for (int ch = 0; ch < nCh; ++ch) {
                auto* dst = owner.renderScratch.getWritePointer(ch);
                const auto* src = returnScratch.getReadPointer(ch);
                for (int i = 0; i < bs; ++i) {
                    float s = src[i] * retGain;
                    dst[i] += s;
                    if (ch == 0) sumRmsReturn += s * s;
                }
            }
            owner.returnTrackA.rmsLevel.store(std::sqrt(sumRmsReturn / bs), std::memory_order_relaxed);

            // ── 5. Plugin processBlock (NO LOCK) ─────────────────────────
            if (owner.activePlugin != nullptr)
            {
                // Collect hardware MIDI for this synthetic block
                juce::MidiBuffer incomingMidi;
                owner.midiCollector.removeNextBlockOfMessages(incomingMidi, bs);

                if (nTracks > 0)
                    incomingMidi.addEvents(owner.audioTracks[0].midiBuffer, 0, bs, 0);

                owner.activePlugin->processBlock(owner.renderScratch, incomingMidi);
            }

            // ── 6. Apply master fader & accumulate master RMS ─────────────
            float sumRmsMaster = 0.0f;
            for (int ch = 0; ch < nCh; ++ch)
            {
                auto* p = owner.renderScratch.getWritePointer(ch);
                for (int i = 0; i < bs; ++i)
                {
                    p[i] *= mGain;
                    sumRmsMaster += p[i] * p[i];
                }
            }

            owner.masterTrack.rmsLevel.store(
                std::sqrt(sumRmsMaster / (bs * nCh)), std::memory_order_relaxed);

            // ── 7. Advance render position counter ────────────────────────
            owner.appBuffer.renderBlockPosition.fetch_add(bs, std::memory_order_release);

            // ── 8. Push rendered block into the ring ──────────────────────
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
        if (selectedTrackIndex < 0) return;
        if (selectedSceneIndex < 0) return;

        auto& clip = clipGrid[selectedTrackIndex][selectedSceneIndex];

        if (trackInstruments[selectedTrackIndex] == "DrumRack") {
            auto& pad = clip.drumPatterns[selectedDrumPadIndex];
            pad.steps = n;
            pad.pulses = k;
            pad.hitMap.clear();
            regenerateDrumRackMidi(clip);
            sessionView.setClipData(selectedTrackIndex, selectedSceneIndex, clip);

            // Only reschedule if this clip is already playing
            if (clip.isPlaying) {
                double scheduleTime = transportClock.getIsPlaying() ? transportClock.getNextBarPosition() : 0.0;
                auto* p = midiPool.rentPattern();
                p->setNotes(clip.midiNotes, clip.patternLengthBars);
                audioTracks[selectedTrackIndex].commandQueue.push(
                    { TrackCommand::Type::PlayPattern, p, scheduleTime });
            }
        } else {
            clip.euclideanSteps = n;
            clip.euclideanPulses = k;
            clip.hitMap.clear(); // will be regenerated on launch

            // Only reschedule if this clip is already playing
            if (clip.isPlaying) {
                double scheduleTime = transportClock.getIsPlaying() ? transportClock.getNextBarPosition() : 0.0;
                auto* p = euclideanPool.rentPattern();
                p->generate(k, n);
                clip.hitMap.assign(p->getHitMap().begin(), p->getHitMap().end());
                audioTracks[selectedTrackIndex].commandQueue.push(
                    { TrackCommand::Type::PlayPattern, p, scheduleTime });
            }
        }
    };

    patternEditor.onEuclideanHitMapChanged = [this](const std::vector<uint8_t>& map) {
        if (selectedTrackIndex < 0) return;
        if (selectedSceneIndex < 0) return;

        auto& clip = clipGrid[selectedTrackIndex][selectedSceneIndex];

        if (trackInstruments[selectedTrackIndex] == "DrumRack") {
            auto& pad = clip.drumPatterns[selectedDrumPadIndex];
            pad.hitMap = map;
            regenerateDrumRackMidi(clip);
            sessionView.setClipData(selectedTrackIndex, selectedSceneIndex, clip);

            // Only reschedule if this clip is already playing
            if (clip.isPlaying) {
                double scheduleTime = transportClock.getIsPlaying() ? transportClock.getNextBarPosition() : 0.0;
                auto* p = midiPool.rentPattern();
                p->setNotes(clip.midiNotes, clip.patternLengthBars);
                audioTracks[selectedTrackIndex].commandQueue.push(
                    { TrackCommand::Type::PlayPattern, p, scheduleTime });
            }
        } else {
            clip.hitMap = map;

            // Only reschedule if this clip is already playing
            if (clip.isPlaying) {
                double scheduleTime = transportClock.getIsPlaying() ? transportClock.getNextBarPosition() : 0.0;
                auto* p = euclideanPool.rentPattern();
                p->setHitMap(map);
                audioTracks[selectedTrackIndex].commandQueue.push(
                    { TrackCommand::Type::PlayPattern, p, scheduleTime });
            }
        }
    };

    patternEditor.onMidiNotesChanged = [this](const std::vector<MidiNote>& notes) {
        if (selectedTrackIndex < 0) return;
        if (selectedSceneIndex < 0) return;

        // Auto-compute pattern length from note content:
        // find the furthest beat (note end) and round up to the nearest bar.
        double maxBeat = 0.0;
        for (const auto& n : notes)
            maxBeat = std::max(maxBeat, n.startBeat + n.lengthBeats);
        const double autoLengthBars = std::max(1.0, std::ceil(maxBeat / 4.0));

        // Always persist the edited notes to the clip data model
        auto& clip = clipGrid[selectedTrackIndex][selectedSceneIndex];
        clip.midiNotes         = notes;
        clip.patternLengthBars = autoLengthBars;

        // Only push to the audio engine if this clip is already playing.
        // This lets the user edit patterns freely without auto-starting playback.
        if (clip.isPlaying) {
            double scheduleTime = transportClock.getIsPlaying() ? transportClock.getNextBarPosition() : 0.0;

            // FlushNotes: immediately release any voices triggered by the old pattern
            // to prevent stuck notes when a note is deleted while the synth is playing.
            audioTracks[selectedTrackIndex].commandQueue.push(
                { TrackCommand::Type::FlushNotes, nullptr, -1.0 });

            // Schedule the updated pattern at the next bar boundary for clean timing.
            auto* p = midiPool.rentPattern();
            p->setNotes(notes, autoLengthBars);
            audioTracks[selectedTrackIndex].commandQueue.push(
                { TrackCommand::Type::PlayPattern, p, scheduleTime });
        }
    };

    patternEditor.onModeChanged = [this](const juce::String& mode) {
        if (selectedTrackIndex >= 0 && selectedSceneIndex >= 0) {
            clipGrid[selectedTrackIndex][selectedSceneIndex].patternMode = mode;
        }
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
        clip.midiNotes.clear();
        clip.patternMode     = (trackInstruments[t] == "Oscillator") ? "pianoroll" : 
                               ((trackInstruments[t] == "DrumRack") ? "drumrack" : "euclidean");
        sessionView.setClipData(t, s, clip);
        selectedTrackIndex = t;
        selectedSceneIndex = s;
        sessionView.setClipSelected(t, s);
        sessionView.setTrackSelected(t);
        
        if (trackInstruments[t] == "DrumRack") {
            updateDrumRackPatternEditor();
        } else {
            patternEditor.loadClipData(clip);
        }
        
        resized();
    };

    sessionView.onSelectClip = [this](int t, int s) {
        selectedTrackIndex = t;
        selectedSceneIndex = s;
        sessionView.setClipSelected(t, s);
        sessionView.setTrackSelected(t);
        
        if (trackInstruments[t] == "DrumRack") {
            updateDrumRackPatternEditor();
        } else {
            patternEditor.loadClipData(clipGrid[t][s]);
        }
        
        resized();
        showDeviceEditorForTrack(t);
    };

    sessionView.onLaunchClip = [this](int t, int s) {
        sessionView.onSelectClip(t, s);

        double scheduleTime = transportClock.getIsPlaying() ? transportClock.getNextBarPosition() : 0.0;

        if (!transportClock.getIsPlaying()) {
            transportClock.stop(); // to ensure playhead zero
            transportClock.play();
        }

        auto& clip = clipGrid[t][s];
        Pattern* p = nullptr;
        
        if (clip.patternMode == "pianoroll" || clip.patternMode == "drumrack") {
            auto* mp = midiPool.rentPattern();
            mp->setNotes(clip.midiNotes, clip.patternLengthBars);
            p = mp;
        } else {
            auto* ep = euclideanPool.rentPattern();
            if (!clip.hitMap.empty()) {
                ep->setHitMap(clip.hitMap);
            } else {
                ep->generate(clip.euclideanPulses, clip.euclideanSteps);
                clip.hitMap.assign(ep->getHitMap().begin(), ep->getHitMap().end());
            }
            p = ep;
        }
        
        // Stop visually playing clip in the track
        for (int i = 0; i < NUM_SCENES; ++i) {
            if (i != s && clipGrid[t][i].isPlaying) {
                clipGrid[t][i].isPlaying = false;
                sessionView.setClipData(t, i, clipGrid[t][i]);
            }
        }

        audioTracks[t].commandQueue.push(
            { TrackCommand::Type::PlayPattern, p, scheduleTime });
        clip.isPlaying = true;
        sessionView.setClipData(t, s, clip);
    };

    sessionView.onSelectTrack = [this](int t) {
        selectedTrackIndex = t;
        selectedSceneIndex = -1;
        sessionView.setClipSelected(t, -1);
        sessionView.setTrackSelected(t);
        resized();
        showDeviceEditorForTrack(t);
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
            trackInstruments[t] = trackInstruments[t + 1];
            for (int s = 0; s < NUM_SCENES; ++s) {
                clipGrid[t][s] = clipGrid[t + 1][s];
            }
        }

        int last = nTracks - 1;
        audioTracks[last].clear();
        trackInstruments[last] = "";
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
        double schedSample = transportClock.getIsPlaying() ? transportClock.getNextBarPosition() : 0.0;

        if (!transportClock.getIsPlaying()) {
            transportClock.stop(); // zero playhead
            transportClock.play();
        }

        int nTracks = numActiveTracks.load(std::memory_order_relaxed);
        for (int t = 0; t < nTracks; ++t) {
            // Stop visually playing clip in the track
            for (int s = 0; s < NUM_SCENES; ++s) {
                if (s != sceneIdx && clipGrid[t][s].isPlaying) {
                    clipGrid[t][s].isPlaying = false;
                    sessionView.setClipData(t, s, clipGrid[t][s]);
                }
            }

            if (clipGrid[t][sceneIdx].hasClip) {
                auto& clip = clipGrid[t][sceneIdx];
                Pattern* p = nullptr;

                if (clip.patternMode == "pianoroll" || clip.patternMode == "drumrack") {
                    auto* mp = midiPool.rentPattern();
                    mp->setNotes(clip.midiNotes, clip.patternLengthBars);
                    p = mp;
                } else {
                    auto* ep = euclideanPool.rentPattern();
                    if (!clip.hitMap.empty()) {
                        ep->setHitMap(clip.hitMap);
                    } else {
                        ep->generate(clip.euclideanPulses, clip.euclideanSteps);
                        clip.hitMap.assign(ep->getHitMap().begin(), ep->getHitMap().end());
                    }
                    p = ep;
                }

                audioTracks[t].commandQueue.push({ TrackCommand::Type::PlayPattern, p, schedSample });
                clip.isPlaying = true;
                sessionView.setClipData(t, sceneIdx, clip);
            } else {
                audioTracks[t].commandQueue.push({ TrackCommand::Type::StopPattern, nullptr, schedSample });
            }
        }
        sessionView.setSceneActive(sceneIdx, true);
    };

    sessionView.onInstrumentDropped = [this](int trackIdx, const juce::String& type) {
        int nTracks = numActiveTracks.load(std::memory_order_relaxed);
        int targetIdx = -1;

        if (trackIdx >= 0 && trackIdx < nTracks) {
            // Drop onto existing track — update its instrument
            targetIdx = trackIdx;
            sessionView.gridContent.columns[targetIdx]->header.hasInstrument = true;
            sessionView.gridContent.columns[targetIdx]->header.instrumentName = type;
            trackInstruments[targetIdx] = type;
            sessionView.gridContent.columns[targetIdx]->header.repaint();
        } else {
            // Drop onto drop zone — create new track
            if (nTracks >= MAX_TRACKS) return;
            targetIdx = numActiveTracks.fetch_add(1, std::memory_order_relaxed);
            sessionView.addTrack(TrackType::Audio, "Track " + juce::String(targetIdx + 1));
            sessionView.gridContent.columns[targetIdx]->header.hasInstrument = true;
            sessionView.gridContent.columns[targetIdx]->header.instrumentName = type;
            trackInstruments[targetIdx] = type;
            sessionView.gridContent.columns[targetIdx]->header.repaint();
        }

        // Always select the track and show its device panel immediately,
        // regardless of what was previously selected.
        selectedTrackIndex = targetIdx;
        selectedSceneIndex = -1;
        sessionView.setTrackSelected(targetIdx);
        sessionView.setClipSelected(targetIdx, -1);

        // Instantiate modular instrument
        auto newInst = InstrumentFactory::create(type);
        if (newInst) {
            newInst->prepareToPlay(currentSampleRate);
            if (auto* old = audioTracks[targetIdx].activeInstrument.exchange(newInst.release(), std::memory_order_acq_rel)) {
                audioTracks[targetIdx].instrumentGarbageQueue.push(old);
            }
        }

        showDeviceEditorForTrack(targetIdx);

        resized();
    };

    sessionView.onEffectDropped = [this](int trackIdx, const juce::String& type) {
        int nTracks = numActiveTracks.load(std::memory_order_relaxed);
        Track* targetTrack = nullptr;

        if (trackIdx == 999) {
            targetTrack = &returnTrackA;
        } else if (trackIdx >= 0 && trackIdx < nTracks) {
            targetTrack = &audioTracks[trackIdx];
        } else {
            return; // Dropped on an invalid place
        }

        auto newEffect = EffectFactory::create(type);
        if (newEffect) {
            newEffect->prepareToPlay(currentSampleRate);

            // Find an empty slot
            for (int i = 0; i < Track::MAX_EFFECTS; ++i) {
                if (targetTrack->effectChain[i].load(std::memory_order_relaxed) == nullptr) {
                    targetTrack->effectChain[i].store(newEffect.release(), std::memory_order_release);
                    break;
                }
            }
        }

        // Always select the track and show its device panel immediately
        selectedTrackIndex = trackIdx;
        if (trackIdx != 999) selectedSceneIndex = -1;
        sessionView.setTrackSelected(trackIdx);
        if (trackIdx != 999) sessionView.setClipSelected(trackIdx, -1);
        
        showDeviceEditorForTrack(trackIdx);
        resized();
    };

    // ── Mixer Volume Callbacks ────────────────────────────────────────
    // These are the ONLY paths that write to the audio-thread gain atomics.
    // Without them the faders are purely cosmetic.
    sessionView.onTrackVolumeChanged = [this](int t, float gain) {
        if (t >= 0 && t < MAX_TRACKS)
            audioTracks[t].gain.store(gain, std::memory_order_relaxed);
    };

    sessionView.onTrackSendChanged = [this](int t, float level) {
        if (t >= 0 && t < MAX_TRACKS)
            audioTracks[t].sendALevel.store(level, std::memory_order_relaxed);
    };

    sessionView.onMasterVolumeChanged = [this](float gain) {
        masterTrack.gain.store(gain, std::memory_order_relaxed);
    };

    sessionView.onReturnVolumeChanged = [this](float gain) {
        returnTrackA.gain.store(gain, std::memory_order_relaxed);
    };

    sessionView.onTrackMuteChanged = [this](int t, bool muted) {
        if (t >= 0 && t < MAX_TRACKS)
            audioTracks[t].muted.store(muted, std::memory_order_relaxed);
    };

    sessionView.onTrackSoloChanged = [this](int t, bool soloed) {
        if (t >= 0 && t < MAX_TRACKS)
            audioTracks[t].soloed.store(soloed, std::memory_order_relaxed);
    };

    // ── Rename / Color callbacks ──────────────────────────────────────────────
    sessionView.onRenameClip = [this](int t, int s, const juce::String& name) {
        if (juce::isPositiveAndBelow(t, MAX_TRACKS) && juce::isPositiveAndBelow(s, NUM_SCENES)) {
            clipGrid[t][s].name = name;
            sessionView.setClipData(t, s, clipGrid[t][s]);
        }
    };

    sessionView.onSetClipColour = [this](int t, int s, juce::Colour c) {
        if (juce::isPositiveAndBelow(t, MAX_TRACKS) && juce::isPositiveAndBelow(s, NUM_SCENES)) {
            clipGrid[t][s].colour = c;
            sessionView.setClipData(t, s, clipGrid[t][s]);
        }
    };

    sessionView.onRenameTrack = [this](int t, const juce::String& name) {
        // The TrackHeader already updated its own trackName and repainted;
        // nothing else to do at runtime (persistence happens on Save).
        juce::ignoreUnused(t, name);
    };

    sessionView.onSetTrackColour = [this](int t, juce::Colour c) {
        // The TrackHeader already updated its own trackColour and repainted.
        juce::ignoreUnused(t, c);
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
        
    for (int t = 0; t < MAX_TRACKS; ++t) {
        if (auto* inst = audioTracks[t].activeInstrument.load(std::memory_order_acquire)) {
            inst->prepareToPlay(sampleRate);
        }
    }
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

    // ── Dynamic bottom-panel height ───────────────────────────────────────────
    // Read the preferred height from whichever editor is currently loaded.
    // Each instrument/effect editor sets its own height via setSize() in its
    // constructor, so getFirstEditor()->getHeight() is the source of truth.
    // We clamp between 200 and 430 px so the session view always gets room.
    constexpr int kMinDeviceH  = 200;
    constexpr int kMaxDeviceH  = 430;
    constexpr int kDefaultDevH = 260;
    constexpr int kPadding     = 8;

    int editorH = kDefaultDevH;
    if (auto* ed = deviceView.getFirstEditor())
        editorH = juce::jlimit(kMinDeviceH, kMaxDeviceH, ed->getHeight() + kPadding);

    auto bottomPanel = bounds.removeFromBottom(editorH);

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

    if (nTracks > 0) {
        float peakTrackRms = 0.0f;
        for (int t = 0; t < nTracks; ++t)
            peakTrackRms = std::max(peakTrackRms, audioTracks[t].rmsLevel.load(std::memory_order_relaxed));
        applyDecay(audioLevelDisplay, peakTrackRms);
    }

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

    // ── GC: Instrument internal buffers & Effect garbage ─────────────────────
    auto gcProcessors = [](Track& t) {
        if (auto* inst = t.activeInstrument.load(std::memory_order_relaxed)) {
            inst->processGarbage();
        }
        for (int i = 0; i < Track::MAX_EFFECTS; ++i) {
            if (auto* effect = t.effectChain[i].load(std::memory_order_relaxed)) {
                effect->processGarbage();
            }
        }
        while (auto opt = t.effectGarbageQueue.pop()) {
            delete *opt;
        }
    };
    
    for (int t = 0; t < nTracks; ++t) {
        gcProcessors(audioTracks[t]);
    }
    gcProcessors(returnTrackA);
    gcProcessors(masterTrack);

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

    // ── Push RMS + gain into mixer strip columns (message thread, no lock) ───
    for (int t = 0; t < nTracks; ++t)
    {
        if (juce::isPositiveAndBelow (t, sessionView.gridContent.columns.size()))
        {
            auto* col = sessionView.gridContent.columns[t];
            float newRms  = audioTracks[t].rmsLevel.load (std::memory_order_relaxed);
            float newGain = audioTracks[t].gain.load     (std::memory_order_relaxed);
            col->rmsLevel  = (newRms > col->rmsLevel) ? newRms
                                                       : col->rmsLevel * 0.75f + newRms * 0.25f;
            col->gainValue = newGain;
            col->repaintMixerStrip();
        }
    }

    // ── Push RMS into Return A and Master faders ──────────────────────────────
    {
        float retRaw    = returnTrackA.rmsLevel.load (std::memory_order_relaxed);
        float masterRaw = masterTrack.rmsLevel.load  (std::memory_order_relaxed);
        applyDecay (returnLevelDisplay, retRaw);
        applyDecay (masterLevelDisplay, masterRaw);
        sessionView.setReturnRmsLevel (returnLevelDisplay);
        sessionView.setMasterRmsLevel (masterLevelDisplay);
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
        // Zero all clip grids
        for (int s = 0; s < NUM_SCENES; s++) {
            clipGrid[t][s] = ClipData();
            sessionView.setClipData(t, s, clipGrid[t][s]);
        }
        audioTracks[t].gain.store(1.0f);
        audioTracks[t].commandQueue.push({ TrackCommand::Type::StopPattern, nullptr, 0 });
        for (int e = 0; e < Track::MAX_EFFECTS; ++e) {
            if (auto* old = audioTracks[t].effectChain[e].exchange(nullptr, std::memory_order_acq_rel)) {
                audioTracks[t].effectGarbageQueue.push(old);
            }
        }
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

                    // Restore track colour
                    auto argbProp = trackNode.getProperty("colour");
                    juce::Colour tcol = argbProp.isVoid()
                        ? juce::Colour (0xff2d89ef)
                        : juce::Colour ((juce::uint32)(juce::int64)argbProp);

                    sessionView.addTrack(TrackType::Audio, name);

                    // Restore fader position & gain overlay to match saved value
                    if (juce::isPositiveAndBelow (tIdx, sessionView.gridContent.columns.size()))
                    {
                        sessionView.gridContent.columns[tIdx]->volFader.setValue (gain, juce::dontSendNotification);
                        sessionView.gridContent.columns[tIdx]->gainValue = gain;
                        sessionView.gridContent.columns[tIdx]->header.trackColour = tcol;
                        sessionView.gridContent.columns[tIdx]->header.repaint();
                    }
                    
                    juce::String instrumentType = trackNode.getProperty("instrument_type", "");
                    trackInstruments[tIdx] = instrumentType;

                    if (instrumentType.isNotEmpty()) {
                        sessionView.gridContent.columns[tIdx]->header.hasInstrument = true;
                        sessionView.gridContent.columns[tIdx]->header.instrumentName = instrumentType;
                        
                        auto newInst = InstrumentFactory::create(instrumentType);
                        if (newInst) {
                            newInst->prepareToPlay(currentSampleRate);
                            
                            if (auto* old = audioTracks[tIdx].activeInstrument.exchange(newInst.release(), std::memory_order_acq_rel)) {
                                audioTracks[tIdx].instrumentGarbageQueue.push(old);
                            }

                            auto* currentInst = audioTracks[tIdx].activeInstrument.load(std::memory_order_acquire);

                            if (instrumentType == "Oscillator") {
                                auto oscNode = trackNode.getChildWithName("OscillatorState");
                                if (oscNode.isValid()) currentInst->loadState(oscNode);
                            } else if (instrumentType == "Simpler") {
                                auto samplerNode = trackNode.getChildWithName("Sampler");
                                if (samplerNode.isValid()) {
                                    juce::String filePath = samplerNode.getProperty("file_path", "");
                                    juce::File f(filePath);
                                    if (f.existsAsFile()) {
                                        currentInst->loadFile(f);
                                        loadAudioFileIntoTrack(tIdx, f);
                                    }
                                }
                            } else if (instrumentType == "DrumRack") {
                                auto stateNode = trackNode.getChildWithName("DrumRackState");
                                if (stateNode.isValid()) {
                                    currentInst->loadState(stateNode);
                                    if (auto* dr = dynamic_cast<DrumRackProcessor*>(currentInst)) {
                                        for (int i = 0; i < 16; ++i) {
                                            if (dr->settings[i].loadedFile.existsAsFile()) {
                                                loadAudioFileIntoDrumPad(tIdx, i, dr->settings[i].loadedFile);
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                    
                    // Effects
                    auto effectsNode = trackNode.getChildWithName("Effects");
                    if (effectsNode.isValid()) {
                        int eIdx = 0;
                        for (int e = 0; e < effectsNode.getNumChildren() && eIdx < Track::MAX_EFFECTS; ++e) {
                            auto effectNode = effectsNode.getChild(e);
                            if (effectNode.hasType("Effect")) {
                                juce::String type = effectNode.getProperty("type", "");
                                if (auto effect = EffectFactory::create(type)) {
                                    effect->prepareToPlay(currentSampleRate);
                                    if (effectNode.getNumChildren() > 0) {
                                        effect->loadState(effectNode.getChild(0));
                                    }
                                    if (auto* old = audioTracks[tIdx].effectChain[eIdx].exchange(effect.release(), std::memory_order_acq_rel)) {
                                        audioTracks[tIdx].effectGarbageQueue.push(old);
                                    }
                                    eIdx++;
                                }
                            }
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
                                    d.hasClip  = clipNode.getProperty("hasClip", false);
                                    d.isPlaying = false;
                                    d.name     = clipNode.getProperty("name", "Pattern " + juce::String(sIdx + 1));
                                    // Restore clip colour
                                    auto clipArgbProp = clipNode.getProperty("colour");
                                    d.colour = clipArgbProp.isVoid()
                                        ? juce::Colour (0xff2d89ef)
                                        : juce::Colour ((juce::uint32)(juce::int64)clipArgbProp);
                                    d.euclideanSteps  = clipNode.getProperty("euclideanSteps",  16);
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

                                    // Restore MidiNotes and Pattern Mode
                                    d.patternMode = clipNode.getProperty("patternMode", "euclidean");
                                    d.patternLengthBars = clipNode.getProperty("patternLengthBars", 1.0);
                                    
                                    auto midiNotesNode = clipNode.getChildWithName("MidiNotes");
                                    if (midiNotesNode.isValid()) {
                                        d.midiNotes.clear();
                                        for (int mn = 0; mn < midiNotesNode.getNumChildren(); ++mn) {
                                            auto noteNode = midiNotesNode.getChild(mn);
                                            MidiNote n;
                                            n.note = noteNode.getProperty("note");
                                            n.startBeat = noteNode.getProperty("startBeat");
                                            n.lengthBeats = noteNode.getProperty("lengthBeats");
                                            n.velocity = noteNode.getProperty("velocity");
                                            d.midiNotes.push_back(n);
                                        }
                                    }
                                    
                                    auto drNode = clipNode.getChildWithName("DrumPatterns");
                                    if (drNode.isValid()) {
                                        for (int p = 0; p < drNode.getNumChildren(); ++p) {
                                            auto pNode = drNode.getChild(p);
                                            int idx = pNode.getProperty("padIndex", -1);
                                            if (idx >= 0 && idx < 16) {
                                                auto& pad = d.drumPatterns[idx];
                                                pad.steps = pNode.getProperty("steps", 16);
                                                pad.pulses = pNode.getProperty("pulses", 0);
                                                juce::String hex = pNode.getProperty("hitMap", "");
                                                if (hex.isNotEmpty() && hex.length() % 2 == 0) {
                                                    pad.hitMap.clear();
                                                    for (int ci = 0; ci < hex.length(); ci += 2)
                                                        pad.hitMap.push_back((uint8_t)hex.substring(ci, ci + 2).getHexValue32());
                                                }
                                            }
                                        }
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

    // Return Track A
    for (int e = 0; e < Track::MAX_EFFECTS; ++e) {
        if (auto* old = returnTrackA.effectChain[e].exchange(nullptr, std::memory_order_acq_rel)) {
            returnTrackA.effectGarbageQueue.push(old);
        }
    }
    auto returnTrackNode = pTree.getChildWithName("ReturnTrackA");
    if (returnTrackNode.isValid()) {
        float gain = returnTrackNode.getProperty("gain", 1.0f);
        returnTrackA.gain.store(gain, std::memory_order_relaxed);
        
        auto returnEffectsNode = returnTrackNode.getChildWithName("Effects");
        if (returnEffectsNode.isValid()) {
            int eIdx = 0;
            for (int e = 0; e < returnEffectsNode.getNumChildren() && eIdx < Track::MAX_EFFECTS; ++e) {
                auto effectNode = returnEffectsNode.getChild(e);
                if (effectNode.hasType("Effect")) {
                    juce::String type = effectNode.getProperty("type", "");
                    if (auto effect = EffectFactory::create(type)) {
                        effect->prepareToPlay(currentSampleRate);
                        if (effectNode.getNumChildren() > 0) {
                            effect->loadState(effectNode.getChild(0));
                        }
                        if (auto* old = returnTrackA.effectChain[eIdx].exchange(effect.release(), std::memory_order_acq_rel)) {
                            returnTrackA.effectGarbageQueue.push(old);
                        }
                        eIdx++;
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
        juce::Colour  tcol (0xff2d89ef);
        if (t < sessionView.gridContent.columns.size()) {
            name = sessionView.gridContent.columns[t]->header.trackName;
            tcol = sessionView.gridContent.columns[t]->header.trackColour;
        }
        trackNode.setProperty("name",   name,                  nullptr);
        trackNode.setProperty("colour", (juce::int64)tcol.getARGB(), nullptr);
        trackNode.setProperty("gain", audioTracks[t].gain.load(std::memory_order_relaxed), nullptr);

        trackNode.setProperty("instrument_type", trackInstruments[t], nullptr);

        if (auto* inst = audioTracks[t].activeInstrument.load(std::memory_order_acquire)) {
            if (trackInstruments[t] == "Oscillator" || trackInstruments[t] == "DrumRack") {
                auto state = inst->saveState();
                trackNode.addChild(state, -1, nullptr);
            } else if (trackInstruments[t] == "Simpler") {
                if (auto* simpler = dynamic_cast<SimplerProcessor*>(inst)) {
                    if (simpler->loadedFile.existsAsFile()) {
                        juce::ValueTree samplerNode("Sampler");
                        samplerNode.setProperty("file_path", simpler->loadedFile.getFullPathName(), nullptr);
                        trackNode.addChild(samplerNode, -1, nullptr);
                    }
                }
            }
        }

        juce::ValueTree clipsNode("Clips");
        for (int s = 0; s < NUM_SCENES; ++s) {
            auto& clip = clipGrid[t][s];
            if (clip.hasClip) {
                juce::ValueTree clipNode("Clip");
                clipNode.setProperty("scene",          s,          nullptr);
                clipNode.setProperty("hasClip",         true,       nullptr);
                clipNode.setProperty("name",            clip.name,  nullptr);
                clipNode.setProperty("colour", (juce::int64)clip.colour.getARGB(), nullptr);
                clipNode.setProperty("euclideanSteps",  clip.euclideanSteps,  nullptr);
                clipNode.setProperty("euclideanPulses", clip.euclideanPulses, nullptr);
                // Serialize hitMap as a hex string so custom patterns survive save/load.
                if (!clip.hitMap.empty()) {
                    juce::String hex;
                    for (uint8_t b : clip.hitMap)
                        hex += juce::String::toHexString(b).paddedLeft('0', 2);
                    clipNode.setProperty("hitMap", hex, nullptr);
                }
                clipsNode.addChild(clipNode, -1, nullptr);

                clipNode.setProperty("patternMode", clip.patternMode, nullptr);
                clipNode.setProperty("patternLengthBars", clip.patternLengthBars, nullptr);

                if (!clip.midiNotes.empty()) {
                    juce::ValueTree midiNotesNode("MidiNotes");
                    for (const auto& n : clip.midiNotes) {
                        juce::ValueTree noteNode("Note");
                        noteNode.setProperty("note", n.note, nullptr);
                        noteNode.setProperty("startBeat", n.startBeat, nullptr);
                        noteNode.setProperty("lengthBeats", n.lengthBeats, nullptr);
                        noteNode.setProperty("velocity", n.velocity, nullptr);
                        midiNotesNode.addChild(noteNode, -1, nullptr);
                    }
                    clipNode.addChild(midiNotesNode, -1, nullptr);
                }

                if (clip.patternMode == "drumrack") {
                    juce::ValueTree drNode("DrumPatterns");
                    for (int p = 0; p < 16; ++p) {
                        auto& pad = clip.drumPatterns[p];
                        if (pad.pulses > 0 || !pad.hitMap.empty()) {
                            juce::ValueTree pNode("PadPattern");
                            pNode.setProperty("padIndex", p, nullptr);
                            pNode.setProperty("steps", pad.steps, nullptr);
                            pNode.setProperty("pulses", pad.pulses, nullptr);
                            if (!pad.hitMap.empty()) {
                                juce::String hex;
                                for (uint8_t b : pad.hitMap) hex += juce::String::toHexString(b).paddedLeft('0', 2);
                                pNode.setProperty("hitMap", hex, nullptr);
                            }
                            drNode.addChild(pNode, -1, nullptr);
                        }
                    }
                    clipNode.addChild(drNode, -1, nullptr);
                }
            }
        }
        if (clipsNode.getNumChildren() > 0)
            trackNode.addChild(clipsNode, -1, nullptr);
            
        // Effects
        juce::ValueTree effectsNode("Effects");
        for (int i = 0; i < Track::MAX_EFFECTS; ++i) {
            if (auto* effect = audioTracks[t].effectChain[i].load(std::memory_order_acquire)) {
                juce::ValueTree effectNode("Effect");
                effectNode.setProperty("type", effect->getName(), nullptr);
                effectNode.addChild(effect->saveState(), -1, nullptr);
                effectsNode.addChild(effectNode, -1, nullptr);
            }
        }
        if (effectsNode.getNumChildren() > 0)
            trackNode.addChild(effectsNode, -1, nullptr);

        tracksTree.addChild(trackNode, -1, nullptr);
    }

    // Return Track A
    juce::ValueTree returnTrackNode("ReturnTrackA");
    returnTrackNode.setProperty("gain", returnTrackA.gain.load(std::memory_order_relaxed), nullptr);
    juce::ValueTree returnEffectsNode("Effects");
    for (int i = 0; i < Track::MAX_EFFECTS; ++i) {
        if (auto* effect = returnTrackA.effectChain[i].load(std::memory_order_acquire)) {
            juce::ValueTree effectNode("Effect");
            effectNode.setProperty("type", effect->getName(), nullptr);
            effectNode.addChild(effect->saveState(), -1, nullptr);
            returnEffectsNode.addChild(effectNode, -1, nullptr);
        }
    }
    if (returnEffectsNode.getNumChildren() > 0)
        returnTrackNode.addChild(returnEffectsNode, -1, nullptr);
    pTree.addChild(returnTrackNode, -1, nullptr);
}

void MainComponent::loadAudioFileIntoTrack(int trackIdx, const juce::File& file)
{
    if (trackIdx < 0 || trackIdx >= MAX_TRACKS) return;

    if (auto* inst = audioTracks[trackIdx].activeInstrument.load(std::memory_order_acquire)) {
        inst->loadFile(file);
    }

    juce::Thread::launch([this, file, trackIdx] {
        if (auto* reader = formatManager.createReaderFor(file)) {
            auto* newBuffer = new juce::AudioBuffer<float>(
                reader->numChannels, (int)reader->lengthInSamples);
            reader->read(newBuffer, 0, (int)reader->lengthInSamples, 0, true, true);
            delete reader;

            juce::MessageManager::callAsync([this, newBuffer, trackIdx, file] {
                if (auto* inst = audioTracks[trackIdx].activeInstrument.load(std::memory_order_acquire)) {
                    if (auto* simpler = dynamic_cast<SimplerProcessor*>(inst)) {
                        simpler->loadNewBuffer(newBuffer);
                    } else {
                        delete newBuffer;
                    }
                } else {
                    delete newBuffer;
                }
                
                if (trackIdx == selectedTrackIndex) {
                    showDeviceEditorForTrack(trackIdx);
                }
            });
        }
    });
}

void MainComponent::regenerateDrumRackMidi(ClipData& clip) {
    clip.midiNotes.clear();
    for (int pad = 0; pad < 16; ++pad) {
        auto& p = clip.drumPatterns[pad];
        if (p.hitMap.empty() && p.pulses == 0) continue;
        
        std::vector<uint8_t> map = p.hitMap;
        if (map.empty() && p.pulses > 0) {
            map.assign(p.steps, 0);
            for (int i = 0; i < p.steps; ++i) {
                map[i] = ((i * p.pulses) % p.steps < p.pulses) ? 1 : 0;
            }
        }
        
        for (int i = 0; i < p.steps; ++i) {
            if (map[i] != 0) {
                MidiNote n;
                n.note = 36 + pad;
                n.startBeat = (double)i * 0.25; // Assuming 16th notes
                n.lengthBeats = 0.25;
                n.velocity = 1.0f;
                clip.midiNotes.push_back(n);
            }
        }
    }
}

void MainComponent::loadAudioFileIntoDrumPad(int trackIdx, int padIndex, const juce::File& file) {
    if (trackIdx < 0 || trackIdx >= MAX_TRACKS) return;

    if (auto* inst = audioTracks[trackIdx].activeInstrument.load(std::memory_order_acquire)) {
        if (auto* dr = dynamic_cast<DrumRackProcessor*>(inst)) {
            juce::Thread::launch([this, file, trackIdx, padIndex] {
                if (auto* reader = formatManager.createReaderFor(file)) {
                    auto* newBuffer = new juce::AudioBuffer<float>(
                        reader->numChannels, (int)reader->lengthInSamples);
                    reader->read(newBuffer, 0, (int)reader->lengthInSamples, 0, true, true);
                    delete reader;

                    juce::MessageManager::callAsync([this, newBuffer, trackIdx, padIndex, file] {
                        if (auto* inst = audioTracks[trackIdx].activeInstrument.load(std::memory_order_acquire)) {
                            if (auto* dr = dynamic_cast<DrumRackProcessor*>(inst)) {
                                dr->loadBufferToPad(padIndex, newBuffer, file);
                            } else {
                                delete newBuffer;
                            }
                        } else {
                            delete newBuffer;
                        }
                    });
                }
            });
        }
    }
}

void MainComponent::showDeviceEditorForTrack(int trackIdx) {
    deviceView.clear();

    Track* track = nullptr;
    if (trackIdx == 999) {
        track = &returnTrackA;
    } else if (trackIdx >= 0 && trackIdx < MAX_TRACKS) {
        track = &audioTracks[trackIdx];
    } else {
        return;
    }

    if (auto* inst = track->activeInstrument.load(std::memory_order_acquire)) {
        auto editor = inst->createEditor();
        
        if (auto* drumComp = dynamic_cast<DrumRackComponent*>(editor.get())) {
            drumComp->onSampleDropped = [this, trackIdx](int padIndex, juce::File file) {
                loadAudioFileIntoDrumPad(trackIdx, padIndex, file);
            };
            drumComp->onPadSelected = [this](int padIndex) {
                selectedDrumPadIndex = padIndex;
                updateDrumRackPatternEditor();
            };
        }
        deviceView.addEditor(std::move(editor));
        
        if (auto* drumComp = dynamic_cast<DrumRackComponent*>(deviceView.getFirstEditor())) {
            if (drumComp->onPadSelected) drumComp->onPadSelected(0); // Select Pad 1 by default
        }
    }

    // Add Effect Editors
    for (int i = 0; i < Track::MAX_EFFECTS; ++i) {
        if (auto* effect = track->effectChain[i].load(std::memory_order_acquire)) {
            class EffectWrapper : public juce::Component {
            public:
                std::function<void()> onRemove;
                std::unique_ptr<juce::Component> content;
                juce::TextButton removeBtn{"X"};

                EffectWrapper(std::unique_ptr<juce::Component> ed) : content(std::move(ed)) {
                    addAndMakeVisible(content.get());
                    addAndMakeVisible(removeBtn);
                    removeBtn.setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
                    removeBtn.setColour(juce::TextButton::textColourOffId, juce::Colours::grey);
                    removeBtn.onClick = [this] { if (onRemove) onRemove(); };
                    setSize(content->getWidth(), content->getHeight());
                }

                void resized() override {
                    content->setBounds(getLocalBounds());
                    removeBtn.setBounds(getWidth() - 20, 2, 16, 16);
                }
            };
            
            auto wrapper = std::make_unique<EffectWrapper>(effect->createEditor());
            wrapper->onRemove = [this, track, i, trackIdx]() {
                if (auto* old = track->effectChain[i].exchange(nullptr, std::memory_order_acq_rel)) {
                    track->effectGarbageQueue.push(old);
                }
                showDeviceEditorForTrack(trackIdx);
            };
            deviceView.addEditor(std::move(wrapper));
        }
    }
}

void MainComponent::updateDrumRackPatternEditor() {
    if (selectedTrackIndex >= 0 && selectedSceneIndex >= 0) {
        if (trackInstruments[selectedTrackIndex] == "DrumRack") {
            auto& clip = clipGrid[selectedTrackIndex][selectedSceneIndex];
            auto& pad = clip.drumPatterns[selectedDrumPadIndex];
            patternEditor.loadDrumPadData(pad.steps, pad.pulses, pad.hitMap);
        }
    }
}
