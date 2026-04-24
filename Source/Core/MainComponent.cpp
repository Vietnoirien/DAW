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
            while (auto oldArr = owner.arrangementGarbageQueue.pop()) {
                delete *oldArr;
            }

            bool isArrMode = owner.renderIsArrangementMode.load(std::memory_order_acquire);
            auto* currArrangement = owner.renderArrangement.load(std::memory_order_acquire);

            auto evaluateAutomation = [](const std::list<AutomationLane>& lanes, double localBeat, Track* track) {
                if (lanes.empty()) return;
                juce::SpinLock::ScopedTryLockType sl(track->automationLock);
                if (!sl.isLocked() || track->automationRegistry.empty()) return;
                for (const auto& lane : lanes) {
                    if (lane.points.empty()) continue;
                    auto it = track->automationRegistry.find(lane.parameterId);
                    if (it == track->automationRegistry.end() || it->second.ptr == nullptr) continue;

                    float value = lane.points.front().value;
                    if (localBeat <= lane.points.front().positionBeats) {
                        value = lane.points.front().value;
                    } else if (localBeat >= lane.points.back().positionBeats) {
                        value = lane.points.back().value;
                    } else {
                        for (size_t i = 0; i < lane.points.size() - 1; ++i) {
                            if (localBeat >= lane.points[i].positionBeats && localBeat < lane.points[i+1].positionBeats) {
                                const auto& p1 = lane.points[i];
                                const auto& p2 = lane.points[i+1];
                                float ratio = static_cast<float>((localBeat - p1.positionBeats) / (p2.positionBeats - p1.positionBeats));
                                value = p1.value + ratio * (p2.value - p1.value);
                                break;
                            }
                        }
                    }
                    
                    float mappedValue = it->second.minVal + value * (it->second.maxVal - it->second.minVal);
                    it->second.ptr->store(mappedValue, std::memory_order_release);
                }
            };

            for (int t = 0; t < nTracks; ++t) {
                // Always process to drain queue, but we will overwrite midiBuffer if in Arrangement mode
                owner.audioTracks[t]->processPatternCommands(blockStart, bs, owner.transportClock);
                
                if (isArrMode && currArrangement) {
                    owner.audioTracks[t]->midiBuffer.clear(); // Override Session View notes
                    
                    if (!owner.transportClock.getIsPlaying()) {
                        owner.audioTracks[t]->midiBuffer.addEvent(juce::MidiMessage::allNotesOff(1), 0);
                        continue;
                    }

                    int64_t offset = owner.renderTransportOffset.load(std::memory_order_acquire);
                    int64_t transportSample = blockStart - offset;
                    if (transportSample < 0) transportSample = 0;

                    double currentBeat = (static_cast<double>(transportSample) / owner.transportClock.getSamplesPerBeat());
                    double endBeat = currentBeat + (static_cast<double>(bs) / owner.transportClock.getSamplesPerBeat());

                    if (t < (int)currArrangement->tracks.size()) {

                        for (const auto& clip : currArrangement->tracks[t]) {
                            double clipStartBeat = (clip.startBar - 1.0) * 4.0;
                            double clipEndBeat = clipStartBeat + (clip.lengthBars * 4.0);

                            // If clip overlaps this block
                            if (clipEndBeat > currentBeat && clipStartBeat < endBeat) {
                                // Evaluate Arrangement Clip Automation Lanes
                                evaluateAutomation(clip.automationLanes, currentBeat - clipStartBeat, owner.audioTracks[t].get());
                                // Evaluate Clip-level Automation Lanes (fallback if arrangement doesn't override)
                                evaluateAutomation(clip.data.automationLanes, currentBeat - clipStartBeat, owner.audioTracks[t].get());

                                double patternLengthBeats = clip.data.patternLengthBars > 0.0 ? clip.data.patternLengthBars * 4.0 : 4.0;
                                
                                for (const auto& note : clip.data.midiNotes) {
                                    long kStart = std::max(0L, static_cast<long>(std::floor((currentBeat - clipStartBeat - note.startBeat - note.lengthBeats) / patternLengthBeats)));
                                    long kEnd = static_cast<long>(std::ceil((endBeat - clipStartBeat - note.startBeat) / patternLengthBeats));
                                    
                                    for (long k = kStart; k <= kEnd; ++k) {
                                        double noteAbsStart = clipStartBeat + k * patternLengthBeats + note.startBeat;
                                        double noteAbsEnd = noteAbsStart + note.lengthBeats;
                                        
                                        // Ensure the note doesn't play if it starts after the clip ends
                                        if (noteAbsStart >= clipEndBeat) continue;
                                        
                                        // If note is cut off by the end of the clip, trim noteAbsEnd
                                        if (noteAbsEnd > clipEndBeat) noteAbsEnd = clipEndBeat;
                                        
                                        // Note On
                                        if (noteAbsStart >= currentBeat && noteAbsStart < endBeat) {
                                            int sampleOffset = static_cast<int>((noteAbsStart - currentBeat) * owner.transportClock.getSamplesPerBeat());
                                            sampleOffset = juce::jlimit(0, bs - 1, sampleOffset);
                                            owner.audioTracks[t]->midiBuffer.addEvent(
                                                juce::MidiMessage::noteOn(1, note.note, note.velocity), sampleOffset);
                                        }
                                        
                                        // Note Off
                                        if (noteAbsEnd >= currentBeat && noteAbsEnd < endBeat) {
                                            int sampleOffset = static_cast<int>((noteAbsEnd - currentBeat) * owner.transportClock.getSamplesPerBeat());
                                            sampleOffset = juce::jlimit(0, bs - 1, sampleOffset);
                                            owner.audioTracks[t]->midiBuffer.addEvent(
                                                juce::MidiMessage::noteOff(1, note.note, 0.0f), sampleOffset);
                                        }
                                    }
                                }
                            }
                        }
                    }
                } else {
                    // Session View (Pattern Mode) Automation
                    if (owner.transportClock.getIsPlaying() && owner.audioTracks[t]->currentPattern) {
                        double lenBeats = owner.audioTracks[t]->currentPattern->getLengthBeats();
                        if (lenBeats > 0.0) {
                            double elapsedBeats = static_cast<double>(blockStart - owner.audioTracks[t]->patternStartSample) / owner.transportClock.getSamplesPerBeat();
                            if (elapsedBeats >= 0.0) {
                                double localBeat = std::fmod(elapsedBeats, lenBeats);
                                evaluateAutomation(owner.audioTracks[t]->currentPattern->automationLanes, localBeat, owner.audioTracks[t].get());
                            }
                        }
                    }
                }
            }

            // ── 4. Per-track render + gain + sum into master mix ───────────
            // renderScratch is the MASTER MIX accumulator.
            // trackScratch is a temporary per-track render buffer.
            owner.renderScratch.clear();

            // Pre-pass: is any track soloed? (determines whether solo silences others)
            bool anySoloed = false;
            for (int t = 0; t < nTracks; ++t)
                if (owner.audioTracks[t]->soloed.load(std::memory_order_relaxed))
                    { anySoloed = true; break; }

            juce::AudioBuffer<float> trackScratch (nCh, bs);
            juce::AudioBuffer<float> returnScratch (nCh, bs);
            returnScratch.clear();

            for (int t = 0; t < nTracks; ++t)
            {
                const bool muted  = owner.audioTracks[t]->muted.load(std::memory_order_relaxed);
                const bool soloed = owner.audioTracks[t]->soloed.load(std::memory_order_relaxed);
                // A track is audible if: not muted AND (nothing is soloed OR this track is soloed)
                const bool audible = !muted && (!anySoloed || soloed);
                const float tGain = audible
                    ? owner.audioTracks[t]->gain.load(std::memory_order_relaxed)
                    : 0.0f;

                trackScratch.clear();
                if (auto* inst = owner.audioTracks[t]->activeInstrument.load(std::memory_order_acquire)) {
                    inst->processBlock(trackScratch, owner.audioTracks[t]->midiBuffer);
                }

                for (int e = 0; e < Track::MAX_EFFECTS; ++e) {
                    if (auto* effect = owner.audioTracks[t]->effectChain[e].load(std::memory_order_acquire)) {
                        effect->processBlock(trackScratch);
                    }
                }

                // ── Safe instrument GC ────────────────────────────────────────
                // We drain the queue HERE (after processBlock) so the render
                // thread is guaranteed no longer using the old pointer.
                // Deletion is posted to the MESSAGE thread via callAsync because
                // AudioPluginInstance destructors may interact with the message
                // loop (window deletion, VST3 host callbacks, etc.) and would
                // deadlock or crash if called directly on the render thread.
                while (auto opt = owner.audioTracks[t]->instrumentGarbageQueue.pop())
                {
                    InstrumentProcessor* dead = *opt;
                    juce::MessageManager::callAsync ([dead] { delete dead; });
                }

                const float tSendALevel = audible ? owner.audioTracks[t]->sendALevel.load(std::memory_order_relaxed) : 0.0f;

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
                owner.audioTracks[t]->rmsLevel.store(
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
                    incomingMidi.addEvents(owner.audioTracks[0]->midiBuffer, 0, bs, 0);

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

            // ── Pending seek (message thread → render thread) ─────────────
            // If the user clicked the ruler (or called seekTo via any other
            // path), pendingSeekSample will be >= 0.  We consume it exactly
            // once here, re-anchor the transport offset and flush all notes.
            {
                int64_t seekTarget = owner.pendingSeekSample.exchange(-1, std::memory_order_acq_rel);
                if (seekTarget >= 0) {
                    // Re-anchor: offset = blockStart_AFTER_advance - seekTarget
                    // We use (blockStart + bs) because renderBlockPosition will be
                    // incremented below.  This way the NEXT block's transportSample = 0.
                    owner.renderTransportOffset.store(
                        (blockStart + bs) - seekTarget, std::memory_order_release);
                    // Flush all held notes
                    for (int t = 0; t < (int)owner.audioTracks.size(); ++t)
                        owner.audioTracks[t]->midiBuffer.addEvent(
                            juce::MidiMessage::allNotesOff(1), bs - 1);
                }
            }

            // ── Loop-wrap (Arrangement mode only) ────────────────────────
            if (isArrMode && owner.transportClock.getIsPlaying()
                && owner.transportClock.getLoopEnabled())
            {
                double spb = owner.transportClock.getSamplesPerBeat();
                if (spb > 0.0) {
                    int64_t offset = owner.renderTransportOffset.load(std::memory_order_acquire);
                    // Transport position at the END of this block
                    double transportEndSample = static_cast<double>((blockStart + bs) - offset);
                    double loopEndSmp  = static_cast<double>(owner.transportClock.getLoopEnd());
                    double loopStrtSmp = static_cast<double>(owner.transportClock.getLoopStart());

                    if (transportEndSample >= loopEndSmp && loopEndSmp > loopStrtSmp) {
                        // Wrap: set playhead back to loop start for the next block
                        int64_t seekTarget = owner.transportClock.getLoopStart();
                        owner.transportClock.seekTo(seekTarget);
                        // Re-anchor offset: next block's blockStart will be (blockStart + bs)
                        owner.renderTransportOffset.store(
                            (blockStart + bs) - seekTarget, std::memory_order_release);
                        // Flush notes at end of this block
                        for (int t = 0; t < (int)owner.audioTracks.size(); ++t)
                            owner.audioTracks[t]->midiBuffer.addEvent(
                                juce::MidiMessage::allNotesOff(1), bs - 1);
                    }
                }
            }

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
    // ── Pre-reserve dynamic track storage (128 slots) to prevent reallocation
    // during the session. The render thread accesses audioTracks[] by index
    // while the message thread may push_back; reserving enough capacity
    // guarantees no iterator/pointer invalidation up to 128 tracks.
    audioTracks.reserve(128);
    clipGrid.reserve(128);
    trackInstruments.reserve(128);
    loadedFiles.reserve(128);
    arrangementTracks.reserve(128);

    // ── Application Workspace & Settings ───────────────────────────────────────
    initialiseWorkspace();


    formatManager.registerBasicFormats();
    pluginFormatManager.addFormat (std::make_unique<juce::VST3PluginFormat>());
   #if JUCE_LINUX
    pluginFormatManager.addFormat (std::make_unique<juce::LV2PluginFormat>());
   #endif
   #if JUCE_MAC
    pluginFormatManager.addFormat (std::make_unique<juce::AudioUnitPluginFormat>());
   #endif


    topBar = std::make_unique<TopBarComponent>(transportClock, deviceManager);
    addAndMakeVisible(topBar.get());
    addAndMakeVisible(browser);
    addAndMakeVisible(browserResizer);
    addAndMakeVisible(sessionView);
    addAndMakeVisible(arrangementView);
    arrangementView.setVisible(false);
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
        if (trackIdx < 0 || trackIdx >= (int)audioTracks.size()) {
            DBG("onFileDropped: no track selected — ignoring.");
            return;
        }
        loadAudioFileIntoTrack(trackIdx, file);
    };

    deviceView.canAddAutomation = [this]() -> bool {
        if (selectedTrackIndex < 0 || selectedTrackIndex >= (int)audioTracks.size()) return false;
        // Arrangement mode: need a selected clip
        if (currentView == DAWView::Arrangement)
            return selectedArrangementClip != nullptr;
        // Session mode: need a clip in the grid
        if (selectedSceneIndex < 0 || selectedSceneIndex >= NUM_SCENES) return false;
        return clipGrid[selectedTrackIndex][selectedSceneIndex].hasClip;
    };

    deviceView.hasAutomationForParam = [this](const juce::String& paramId) -> bool {
        if (selectedTrackIndex < 0) return false;
        // Arrangement mode: check the selected arrangement clip
        if (currentView == DAWView::Arrangement) {
            if (selectedArrangementClip == nullptr) return false;
            for (const auto& lane : selectedArrangementClip->automationLanes)
                if (lane.parameterId == paramId && !lane.points.empty()) return true;
            return false;
        }
        // Session mode: check the clip grid
        if (selectedSceneIndex < 0 || selectedTrackIndex >= (int)audioTracks.size()
            || selectedSceneIndex >= NUM_SCENES) return false;
        const auto& clip = clipGrid[selectedTrackIndex][selectedSceneIndex];
        for (const auto& lane : clip.automationLanes)
            if (lane.parameterId == paramId && !lane.points.empty()) return true;
        return false;
    };

    deviceView.onParameterTouched = [this](const juce::String& paramId) {
        if (selectedTrackIndex < 0 || selectedTrackIndex >= (int)audioTracks.size()) return;
        activeAutomationParameterId = paramId;
        activeAutomationTrackIdx = selectedTrackIndex;

        if (currentView == DAWView::Arrangement) {
            // ── Arrangement mode: add automation lane to the SELECTED clip ──
            if (selectedArrangementClip == nullptr) return;
            auto& arrClip = *selectedArrangementClip;
            int arrTrackIdx = arrClip.trackIndex;

            // Seed the lane with the current parameter value if it doesn't exist yet
            bool found = false;
            for (auto& lane : arrClip.automationLanes)
                if (lane.parameterId == paramId) { found = true; break; }

            if (!found) {
                float defaultVal = 0.5f;
                if (arrTrackIdx >= 0 && arrTrackIdx < (int)audioTracks.size()) {
                    if (auto* track = audioTracks[arrTrackIdx].get()) {
                        juce::SpinLock::ScopedLockType sl(track->automationLock);
                        auto it = track->automationRegistry.find(paramId);
                        if (it != track->automationRegistry.end() && it->second.ptr != nullptr) {
                            float rawVal = it->second.ptr->load(std::memory_order_relaxed);
                            float range  = it->second.maxVal - it->second.minVal;
                            if (range != 0.0f) defaultVal = (rawVal - it->second.minVal) / range;
                        }
                    }
                }
                arrClip.automationLanes.push_back({paramId, {}});
                double lenBeats = arrClip.lengthBars * 4.0;
                arrClip.automationLanes.back().points.push_back({0.0, defaultVal});
                arrClip.automationLanes.back().points.push_back({lenBeats, defaultVal});
            }

            // Show the overlay for the focused track
            if (arrTrackIdx >= 0 && arrTrackIdx < (int)arrangementTracks.size()) {
                arrangementView.setArrangementAutomation(
                    arrTrackIdx,
                    &arrangementTracks[arrTrackIdx],
                    paramId);
                // Re-push shared arrangement so render thread sees new lanes
                syncArrangementFromSession();
            }
        } else {
            // ── Session mode: show automation in the pattern editor ────────
            if (selectedSceneIndex >= 0) {
                auto& clip = clipGrid[selectedTrackIndex][selectedSceneIndex];
                AutomationLane* targetLane = nullptr;
                for (auto& lane : clip.automationLanes) {
                    if (lane.parameterId == paramId) { targetLane = &lane; break; }
                }
                if (!targetLane) {
                    clip.automationLanes.push_back({paramId, {}});
                    targetLane = &clip.automationLanes.back();

                    float defaultVal = 0.5f;
                    if (auto* track = audioTracks[selectedTrackIndex].get()) {
                        juce::SpinLock::ScopedLockType sl(track->automationLock);
                        auto it = track->automationRegistry.find(paramId);
                        if (it != track->automationRegistry.end() && it->second.ptr != nullptr) {
                            float rawVal = it->second.ptr->load(std::memory_order_relaxed);
                            float range  = it->second.maxVal - it->second.minVal;
                            if (range != 0.0f) defaultVal = (rawVal - it->second.minVal) / range;
                        }
                    }
                    targetLane->points.push_back({0.0, defaultVal});
                    double lenBeats = clip.patternLengthBars > 0.0 ? clip.patternLengthBars * 4.0 : 4.0;
                    targetLane->points.push_back({lenBeats, defaultVal});
                }
                patternEditor.setActiveAutomationLane(targetLane, paramId, clip.patternLengthBars);
            }
        }
        DBG("Parameter focused for automation: " << paramId);

        // ── Immediately refresh the device-view automation markers ─────────────
        // Build the indicator map from whatever lanes are now present for the
        // current context (arrangement clip or session clip).
        std::unordered_map<juce::String, DeviceView::AutomationParamInfo> autoInfo;
        auto collectLanes = [&](const std::list<AutomationLane>& lanes) {
            for (const auto& lane : lanes) {
                if (lane.points.empty()) continue;
                float minV = lane.points[0].value, maxV = lane.points[0].value;
                for (const auto& pt : lane.points) {
                    minV = std::min(minV, pt.value);
                    maxV = std::max(maxV, pt.value);
                }
                autoInfo[lane.parameterId] = { true, minV, maxV };
            }
        };

        if (currentView == DAWView::Arrangement) {
            if (selectedArrangementClip != nullptr)
                collectLanes(selectedArrangementClip->automationLanes);
        } else {
            if (selectedTrackIndex >= 0 && selectedSceneIndex >= 0
                && selectedSceneIndex < NUM_SCENES
                && selectedTrackIndex < (int)clipGrid.size())
            {
                collectLanes(clipGrid[selectedTrackIndex][selectedSceneIndex].automationLanes);
            }
        }
        deviceView.updateAutomationIndicators(autoInfo);
    };

    // ── Transport ─────────────────────────────────────────────────────────────
    topBar->playBtn.onClick = [this] {
        if (currentView == DAWView::Arrangement)
            renderIsArrangementMode.store(true, std::memory_order_release);
        
        if (!transportClock.getIsPlaying()) {
            renderTransportOffset.store(appBuffer.renderBlockPosition.load(std::memory_order_acquire), std::memory_order_release);
        }
        transportClock.play();
    };

    topBar->stopBtn.onClick = [this] {
        transportClock.stop();
        renderTransportOffset.store(appBuffer.renderBlockPosition.load(std::memory_order_acquire), std::memory_order_release);
        int nTracks = numActiveTracks.load(std::memory_order_relaxed);
        for (int t = 0; t < nTracks; ++t)
            audioTracks[t]->commandQueue.push({ TrackCommand::Type::StopPattern, nullptr, 0 });
        for (int t = 0; t < nTracks; ++t)
            for (int s = 0; s < NUM_SCENES; ++s)
                if (clipGrid[t][s].isPlaying) {
                    clipGrid[t][s].isPlaying = false;
                    sessionView.setClipData(t, s, clipGrid[t][s]);
                }
        sessionView.setSceneActive(-1, false);
    };

    // ── View Toggle ───────────────────────────────────────────────────────────
    topBar->onSwitchToSession = [this] {
        switchToView(DAWView::Session);
        topBar->sessionViewBtn.setToggleState(true, juce::dontSendNotification);
        topBar->arrangeViewBtn.setToggleState(false, juce::dontSendNotification);
    };
    topBar->onSwitchToArrangement = [this] {
        switchToView(DAWView::Arrangement);
        topBar->arrangeViewBtn.setToggleState(true, juce::dontSendNotification);
        topBar->sessionViewBtn.setToggleState(false, juce::dontSendNotification);
    };

    // ── Export Audio ──────────────────────────────────────────────────────────
    topBar->onExportAudio = [this] {
        juce::PopupMenu m;
        m.addItem(1, "Export as WAV");
        m.addItem(2, "Export as MP3");
        m.addItem(3, "Export as FLAC");
        m.addItem(4, "Export as OGG");
        
        m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&topBar->projectBtn),
            [this](int result) {
                if (result == 0) return;
                
                juce::String ext = (result == 1) ? "*.wav" : 
                                   (result == 2) ? "*.mp3" : 
                                   (result == 3) ? "*.flac" : "*.ogg";
                juce::String format = (result == 1) ? "WAV" : 
                                      (result == 2) ? "MP3" : 
                                      (result == 3) ? "FLAC" : "OGG";
                                      
                myChooser = std::make_unique<juce::FileChooser>("Export Audio",
                    juce::File::getSpecialLocation(juce::File::userMusicDirectory).getChildFile("export" + ext.substring(1)), ext);
                    
                myChooser->launchAsync(
                    juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::warnAboutOverwriting,
                    [this, format](const juce::FileChooser& fc) {
                        auto file = fc.getResult();
                        if (file != juce::File{}) {
                            // Run the export on a background thread so UI doesn't freeze
                            juce::Thread::launch([this, file, format]() {
                                exportProject(file, format);
                            });
                        }
                    });
            });
    };

    // ── Plugin Browser (Plugins tab in BrowserComponent) ──────────────────────
    // Seed platform-specific default VST3 / AU locations so plugins show up
    // immediately on first launch without the user having to add folders.
    {
        juce::Array<juce::File> defaultPluginDirs;

       #if JUCE_WINDOWS
        defaultPluginDirs.add (juce::File ("C:\\Program Files\\Common Files\\VST3"));
        defaultPluginDirs.add (juce::File ("C:\\Program Files (x86)\\Common Files\\VST3"));
        defaultPluginDirs.add (juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                                          .getChildFile ("VST3"));
       #elif JUCE_MAC
        defaultPluginDirs.add (juce::File ("/Library/Audio/Plug-Ins/VST3"));
        defaultPluginDirs.add (juce::File ("/Library/Audio/Plug-Ins/Components"));
        defaultPluginDirs.add (juce::File::getSpecialLocation (juce::File::userHomeDirectory)
                                          .getChildFile ("Library/Audio/Plug-Ins/VST3"));
        defaultPluginDirs.add (juce::File::getSpecialLocation (juce::File::userHomeDirectory)
                                          .getChildFile ("Library/Audio/Plug-Ins/Components"));
       #else  // Linux
        defaultPluginDirs.add (juce::File ("/usr/lib/vst3"));
        defaultPluginDirs.add (juce::File ("/usr/local/lib/vst3"));
        defaultPluginDirs.add (juce::File::getSpecialLocation (juce::File::userHomeDirectory)
                                          .getChildFile (".vst3"));
        defaultPluginDirs.add (juce::File ("/usr/lib/lv2"));
        defaultPluginDirs.add (juce::File ("/usr/local/lib/lv2"));
        defaultPluginDirs.add (juce::File::getSpecialLocation (juce::File::userHomeDirectory)
                                          .getChildFile (".lv2"));
       #endif

        for (const auto& dir : defaultPluginDirs)
            if (dir.isDirectory())
                browser.addPluginSearchPath (dir);
    }

    browser.getPluginsPanel().onAddFolder = [this] (const juce::File& dir)
    {
        browser.addPluginSearchPath (dir);
        if (userSettings != nullptr)
        {
            userSettings->setValue ("pluginSearchPaths",
                browser.getPluginSearchPaths().joinIntoString (";"));
            userSettings->saveIfNeeded();
        }
        browser.rescanPlugins (pluginFormatManager);
    };

    browser.getPluginsPanel().onRescan = [this]
    {
        browser.rescanPlugins (pluginFormatManager);
    };


    patternEditor.onEuclideanChanged = [this](int k, int n) {
        if (selectedTrackIndex < 0) return;
        if (selectedSceneIndex < 0) return;
        markDirty();

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
                audioTracks[selectedTrackIndex]->commandQueue.push(
                    { TrackCommand::Type::PlayPattern, p, scheduleTime });
            }
        } else {
            clip.euclideanSteps = n;
            clip.euclideanPulses = k;
            clip.hitMap.clear();

            if (clip.isPlaying) {
                double scheduleTime = transportClock.getIsPlaying() ? transportClock.getNextBarPosition() : 0.0;
                auto* p = euclideanPool.rentPattern();
                p->setBars(clip.euclideanBars);
                p->generate(k, n);
                clip.hitMap.assign(p->getHitMap().begin(), p->getHitMap().end());
                audioTracks[selectedTrackIndex]->commandQueue.push(
                    { TrackCommand::Type::PlayPattern, p, scheduleTime });
            }
        }
    };

    patternEditor.onEuclideanBarsChanged = [this](int bars) {
        if (selectedTrackIndex < 0) return;
        if (selectedSceneIndex < 0) return;
        markDirty();

        auto& clip = clipGrid[selectedTrackIndex][selectedSceneIndex];

        if (trackInstruments[selectedTrackIndex] == "DrumRack") {
            auto& pad = clip.drumPatterns[selectedDrumPadIndex];
            pad.bars = bars;
            // DrumRack uses MidiPattern – update the pattern length in bars
            clip.patternLengthBars = bars;
            if (clip.isPlaying) {
                double scheduleTime = transportClock.getIsPlaying() ? transportClock.getNextBarPosition() : 0.0;
                auto* p = midiPool.rentPattern();
                p->setNotes(clip.midiNotes, clip.patternLengthBars);
                audioTracks[selectedTrackIndex]->commandQueue.push(
                    { TrackCommand::Type::PlayPattern, p, scheduleTime });
            }
        } else {
            clip.euclideanBars = bars;

            if (clip.isPlaying) {
                double scheduleTime = transportClock.getIsPlaying() ? transportClock.getNextBarPosition() : 0.0;
                auto* p = euclideanPool.rentPattern();
                p->setBars(bars);
                if (!clip.hitMap.empty())
                    p->setHitMap(clip.hitMap);
                else {
                    p->generate(clip.euclideanPulses, clip.euclideanSteps);
                    clip.hitMap.assign(p->getHitMap().begin(), p->getHitMap().end());
                }
                p->automationLanes = clip.automationLanes;
                audioTracks[selectedTrackIndex]->commandQueue.push(
                    { TrackCommand::Type::PlayPattern, p, scheduleTime });
            }
        }
    };

    patternEditor.onEuclideanHitMapChanged = [this](const std::vector<uint8_t>& map) {
        if (selectedTrackIndex < 0) return;
        if (selectedSceneIndex < 0) return;
        markDirty();

        auto& clip = clipGrid[selectedTrackIndex][selectedSceneIndex];

        if (trackInstruments[selectedTrackIndex] == "DrumRack") {
            auto& pad = clip.drumPatterns[selectedDrumPadIndex];
            pad.hitMap = map;
            regenerateDrumRackMidi(clip);
            sessionView.setClipData(selectedTrackIndex, selectedSceneIndex, clip);

            if (clip.isPlaying) {
                double scheduleTime = transportClock.getIsPlaying() ? transportClock.getNextBarPosition() : 0.0;
                auto* p = midiPool.rentPattern();
                p->setNotes(clip.midiNotes, clip.patternLengthBars);
                audioTracks[selectedTrackIndex]->commandQueue.push(
                    { TrackCommand::Type::PlayPattern, p, scheduleTime });
            }
        } else {
            clip.hitMap = map;

            if (clip.isPlaying) {
                double scheduleTime = transportClock.getIsPlaying() ? transportClock.getNextBarPosition() : 0.0;
                auto* p = euclideanPool.rentPattern();
                p->setBars(clip.euclideanBars);
                p->setHitMap(map);
                audioTracks[selectedTrackIndex]->commandQueue.push(
                    { TrackCommand::Type::PlayPattern, p, scheduleTime });
            }
        }
    };

    patternEditor.onMidiNotesChanged = [this](const std::vector<MidiNote>& notes) {
        if (selectedTrackIndex < 0) return;
        if (selectedSceneIndex < 0) return;
        markDirty();

        double maxBeat = 0.0;
        for (const auto& n : notes)
            maxBeat = std::max(maxBeat, n.startBeat + n.lengthBeats);
        const double autoLengthBars = std::max(1.0, std::ceil(maxBeat / 4.0));

        auto& clip = clipGrid[selectedTrackIndex][selectedSceneIndex];
        clip.midiNotes         = notes;
        clip.patternLengthBars = autoLengthBars;

        if (clip.isPlaying) {
            double scheduleTime = transportClock.getIsPlaying() ? transportClock.getNextBarPosition() : 0.0;
            audioTracks[selectedTrackIndex]->commandQueue.push(
                { TrackCommand::Type::FlushNotes, nullptr, -1.0 });
            auto* p = midiPool.rentPattern();
            p->setNotes(notes, autoLengthBars);
            audioTracks[selectedTrackIndex]->commandQueue.push(
                { TrackCommand::Type::PlayPattern, p, scheduleTime });
        }
    };

    patternEditor.onModeChanged = [this](const juce::String& mode) {
        if (selectedTrackIndex >= 0 && selectedSceneIndex >= 0) {
            if (mode != "automation")
            {
                clipGrid[selectedTrackIndex][selectedSceneIndex].patternMode = mode;
                markDirty();
            }
        }
    };

    patternEditor.onAuditionNoteOn = [this](int note, int vel) {
        if (selectedTrackIndex >= 0 && selectedTrackIndex < (int)audioTracks.size()) {
            TrackCommand cmd;
            cmd.type = TrackCommand::Type::AuditionNoteOn;
            cmd.note = note;
            cmd.velocity = vel;
            audioTracks[selectedTrackIndex]->commandQueue.push(cmd);
        }
    };

    patternEditor.onAuditionNoteOff = [this](int note) {
        if (selectedTrackIndex >= 0 && selectedTrackIndex < (int)audioTracks.size()) {
            TrackCommand cmd;
            cmd.type = TrackCommand::Type::AuditionNoteOff;
            cmd.note = note;
            audioTracks[selectedTrackIndex]->commandQueue.push(cmd);
        }
    };

    patternEditor.onAutomationLaneChanged = [this]() {
        if (selectedTrackIndex < 0 || selectedSceneIndex < 0) return;
        markDirty();
        auto& clip = clipGrid[selectedTrackIndex][selectedSceneIndex];
        
        sessionView.setClipData(selectedTrackIndex, selectedSceneIndex, clip);

        if (clip.isPlaying) {
            double scheduleTime = transportClock.getIsPlaying() ? transportClock.getNextBarPosition() : 0.0;
            Pattern* p = nullptr;
            
            const bool usePianoRoll = (clip.patternMode == "pianoroll" || clip.patternMode == "drumrack"
                                       || (clip.patternMode == "automation" && !clip.midiNotes.empty()));
            if (usePianoRoll) {
                auto* mp = midiPool.rentPattern();
                mp->setNotes(clip.midiNotes, clip.patternLengthBars);
                p = mp;
            } else {
                auto* ep = euclideanPool.rentPattern();
                ep->setBars(clip.euclideanBars);
                ep->setHitMap(clip.hitMap);
                p = ep;
            }
            p->automationLanes = clip.automationLanes;
            
            audioTracks[selectedTrackIndex]->commandQueue.push(
                { TrackCommand::Type::PlayPattern, p, scheduleTime });
        }
    };

    // ── Session View Callbacks ────────────────────────────────────────────────
    sessionView.onCreateClip = [this](int t, int s) {
        if (t < 0 || t >= (int)audioTracks.size()) return;
        markDirty();
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
        
        if (!activeAutomationParameterId.isEmpty()) {
            AutomationLane* targetLane = nullptr;
            for (auto& lane : clip.automationLanes) {
                if (lane.parameterId == activeAutomationParameterId) {
                    targetLane = &lane;
                    break;
                }
            }
            if (!targetLane) {
                clip.automationLanes.push_back({activeAutomationParameterId, {}});
                targetLane = &clip.automationLanes.back();
            }
            patternEditor.setActiveAutomationLane(targetLane, activeAutomationParameterId, clip.patternLengthBars);
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
        
        {
            auto& clip = clipGrid[t][s];
            AutomationLane* targetLane = nullptr;

            if (!activeAutomationParameterId.isEmpty()) {
                // Find existing lane — never create a blank one here if it already exists with points
                for (auto& lane : clip.automationLanes) {
                    if (lane.parameterId == activeAutomationParameterId) {
                        targetLane = &lane;
                        break;
                    }
                }
                // Only create a new lane if this parameter has never been automated for this clip
                if (!targetLane) {
                    float defaultVal = 0.5f;
                    if (auto* track = audioTracks[t].get()) {
                        auto it = track->automationRegistry.find(activeAutomationParameterId);
                        if (it != track->automationRegistry.end() && it->second.ptr != nullptr) {
                            float rawVal = it->second.ptr->load(std::memory_order_relaxed);
                            float range = it->second.maxVal - it->second.minVal;
                            if (range != 0.0f) defaultVal = (rawVal - it->second.minVal) / range;
                        }
                    }
                    clip.automationLanes.push_back({activeAutomationParameterId, {}});
                    targetLane = &clip.automationLanes.back();
                    double lenBeats = clip.patternLengthBars > 0.0 ? clip.patternLengthBars * 4.0 : 4.0;
                    targetLane->points.push_back({0.0, defaultVal});
                    targetLane->points.push_back({lenBeats, defaultVal});
                }
            } else if (!clip.automationLanes.empty()) {
                // No active parameter focused — restore the first lane so the editor isn't blank
                targetLane = &clip.automationLanes.front();
                activeAutomationParameterId = targetLane->parameterId;
                activeAutomationTrackIdx = t;
            }

            if (targetLane) {
                patternEditor.setActiveAutomationLane(targetLane, activeAutomationParameterId, clip.patternLengthBars);
            }
        }

        resized();
        showDeviceEditorForTrack(t);
    };

    sessionView.onLaunchClip = [this](int t, int s) {
        sessionView.onSelectClip(t, s);

        double scheduleTime = transportClock.getIsPlaying() ? transportClock.getNextBarPosition() : 0.0;

        if (!transportClock.getIsPlaying()) {
            transportClock.stop(); // to ensure playhead zero
            renderTransportOffset.store(appBuffer.renderBlockPosition.load(std::memory_order_acquire), std::memory_order_release);
            transportClock.play();
        }

        auto& clip = clipGrid[t][s];
        Pattern* p = nullptr;
        
        // "automation" is a view-only state — resolve to the real MIDI generation mode.
        const bool usePianoRoll = (clip.patternMode == "pianoroll" || clip.patternMode == "drumrack"
                                   || (clip.patternMode == "automation" && !clip.midiNotes.empty()));
        if (usePianoRoll) {
            auto* mp = midiPool.rentPattern();
            mp->setNotes(clip.midiNotes, clip.patternLengthBars);
            p = mp;
        } else {
            auto* ep = euclideanPool.rentPattern();
            ep->setBars(clip.euclideanBars);
            if (!clip.hitMap.empty()) {
                ep->setHitMap(clip.hitMap);
            } else {
                ep->generate(clip.euclideanPulses, clip.euclideanSteps);
                clip.hitMap.assign(ep->getHitMap().begin(), ep->getHitMap().end());
            }
            p = ep;
        }
        
        // Copy automation lanes so the render thread can evaluate them
        p->automationLanes = clip.automationLanes;

        // Stop visually playing clip in the track
        for (int i = 0; i < NUM_SCENES; ++i) {
            if (i != s && clipGrid[t][i].isPlaying) {
                clipGrid[t][i].isPlaying = false;
                sessionView.setClipData(t, i, clipGrid[t][i]);
            }
        }

        audioTracks[t]->commandQueue.push(
            { TrackCommand::Type::PlayPattern, p, scheduleTime });
        clip.isPlaying = true;
        sessionView.setClipData(t, s, clip);
    };

    sessionView.onPauseClip = [this](int t, int s) {
        if (t < 0 || t >= (int)audioTracks.size()) return;
        auto& clip = clipGrid[t][s];
        if (!clip.isPlaying) return;

        audioTracks[t]->commandQueue.push({ TrackCommand::Type::StopPattern, nullptr, 0 });
        clip.isPlaying = false;
        sessionView.setClipData(t, s, clip);

        // If no other clip is playing on any track, stop the transport clock
        bool anyPlaying = false;
        int nTracks = numActiveTracks.load(std::memory_order_relaxed);
        for (int ti = 0; ti < nTracks && !anyPlaying; ++ti)
            for (int si = 0; si < NUM_SCENES && !anyPlaying; ++si)
                if (clipGrid[ti][si].isPlaying) anyPlaying = true;

        if (!anyPlaying)
            transportClock.stop();
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
        if (t < 0 || t >= (int)audioTracks.size()) return;
        auto& clip = clipGrid[t][s];
        if (!clip.hasClip) return;
        markDirty();

        if (clip.isPlaying) {
            audioTracks[t]->commandQueue.push({ TrackCommand::Type::StopPattern, nullptr, 0 });
        }
        clip = ClipData();
        sessionView.setClipData(t, s, clip);
        if (selectedTrackIndex == t && selectedSceneIndex == s) {
            patternEditor.setVisible(false);
            selectedSceneIndex = -1;
            resized();
        }
    };

    sessionView.onDuplicateClip = [this](int t, int s) {
        if (t < 0 || t >= (int)audioTracks.size()) return;
        auto& sourceClip = clipGrid[t][s];
        if (!sourceClip.hasClip) return;
        
        int nextEmpty = -1;
        for (int ns = s + 1; ns < NUM_SCENES; ++ns) {
            if (!clipGrid[t][ns].hasClip) {
                nextEmpty = ns;
                break;
            }
        }
        
        if (nextEmpty != -1) {
            ClipData copy = sourceClip;
            copy.isPlaying = false; 
            clipGrid[t][nextEmpty] = copy;
            sessionView.setClipData(t, nextEmpty, copy);
        }
    };

    sessionView.onDeleteTrack = [this](int trackIndex) {
        int nTracks = numActiveTracks.load(std::memory_order_acquire);
        if (trackIndex < 0 || trackIndex >= nTracks) return;

        // ── Step 1: Atomically detach the instrument BEFORE stopping the thread.
        // This ensures the render thread will see nullptr on its next processBlock
        // call even if it's currently finishing an iteration.  We also close any
        // open plugin UI window here (on the message thread, which is the right place).
        InstrumentProcessor* deadInst = nullptr;
        {
            auto* inst = audioTracks[trackIndex]->activeInstrument.load(std::memory_order_acquire);
            if (inst) inst->closeUI();
            deadInst = audioTracks[trackIndex]->activeInstrument.exchange(
                nullptr, std::memory_order_acq_rel);
        }

        // ── Step 2: Stop the render thread with a generous timeout.
        // Because activeInstrument is already nullptr, the render thread won't
        // enter processBlock for this track — so 2 s is more than enough.
        bool wasRunning = renderThread.isThreadRunning();
        if (wasRunning) renderThread.stopThread(2000);

        // ── Step 3: Async-delete the old instrument on the message thread.
        // This matches the GC path used by the render thread and is safe for
        // PluginInstrumentAdapters whose VST3 destructors need the message loop.
        if (deadInst)
            juce::MessageManager::callAsync([deadInst] { delete deadInst; });

        // ── Step 4: Drain remaining garbage (patterns, effects) synchronously
        // now that the render thread is stopped.
        audioTracks[trackIndex]->clear();

        // Erase from all parallel vectors
        audioTracks.erase(audioTracks.begin() + trackIndex);
        clipGrid.erase(clipGrid.begin() + trackIndex);
        trackInstruments.erase(trackInstruments.begin() + trackIndex);
        loadedFiles.erase(loadedFiles.begin() + trackIndex);
        arrangementTracks.erase(arrangementTracks.begin() + trackIndex);

        numActiveTracks.fetch_sub(1, std::memory_order_release);
        sessionView.removeTrack(trackIndex);

        if (selectedTrackIndex == trackIndex) {
            selectedTrackIndex = -1;
            selectedSceneIndex = -1;
            patternEditor.setVisible(false);
            deviceView.onFileDropped(juce::File());
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
            renderTransportOffset.store(appBuffer.renderBlockPosition.load(std::memory_order_acquire), std::memory_order_release);
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

                const bool usePianoRoll = (clip.patternMode == "pianoroll" || clip.patternMode == "drumrack"
                                           || (clip.patternMode == "automation" && !clip.midiNotes.empty()));
                if (usePianoRoll) {
                    auto* mp = midiPool.rentPattern();
                    mp->setNotes(clip.midiNotes, clip.patternLengthBars);
                    p = mp;
                } else {
                    auto* ep = euclideanPool.rentPattern();
                    ep->setBars(clip.euclideanBars);
                    if (!clip.hitMap.empty()) {
                        ep->setHitMap(clip.hitMap);
                    } else {
                        ep->generate(clip.euclideanPulses, clip.euclideanSteps);
                        clip.hitMap.assign(ep->getHitMap().begin(), ep->getHitMap().end());
                    }
                    p = ep;
                }

                p->automationLanes = clip.automationLanes;
                audioTracks[t]->commandQueue.push({ TrackCommand::Type::PlayPattern, p, schedSample });
                clip.isPlaying = true;
                sessionView.setClipData(t, sceneIdx, clip);
            } else {
                audioTracks[t]->commandQueue.push({ TrackCommand::Type::StopPattern, nullptr, schedSample });
            }
        }
        sessionView.setSceneActive(sceneIdx, true);
    };

    sessionView.onInstrumentDropped = [this](int trackIdx, const juce::String& type) {
        // ── PluginDrag: drag from Plugins browser tab ──────────────────────
        // The drag description is "PluginDrag:<absolutePathToPlugin>"
        if (type.startsWith ("__PluginPath__:"))
        {
            juce::String pluginPath = type.substring (15); // strip prefix
            loadPluginAsTrackInstrument (trackIdx, juce::File (pluginPath));
            return;
        }

        int nTracks = numActiveTracks.load(std::memory_order_relaxed);
        int targetIdx = -1;

        if (trackIdx >= 0 && trackIdx < nTracks) {
            // Drop onto existing track — update its instrument
            targetIdx = trackIdx;
            sessionView.gridContent.columns[targetIdx]->header.hasInstrument = true;
            sessionView.gridContent.columns[targetIdx]->header.instrumentName = type;
            sessionView.gridContent.columns[targetIdx]->header.repaint();
            trackInstruments[targetIdx] = type;
            if (auto* current = audioTracks[targetIdx]->activeInstrument.load(std::memory_order_acquire)) {
                current->closeUI();
            }
            
            // Instantiate modular instrument
            auto newInst = InstrumentFactory::create(type);
            if (newInst) {
                newInst->prepareToPlay(currentSampleRate);
                if (auto* old = audioTracks[targetIdx]->activeInstrument.exchange(newInst.release(), std::memory_order_acq_rel)) {
                    audioTracks[targetIdx]->instrumentGarbageQueue.push(old);
                }
                audioTracks[targetIdx]->refreshAutomationRegistry();
            }
        } else {
            // Drop onto drop zone — create new track (no upper limit)
            // Push to all parallel vectors BEFORE incrementing numActiveTracks so
            // the render thread never sees an out-of-range index.
            audioTracks.push_back(std::make_unique<Track>());
            clipGrid.push_back({});
            trackInstruments.push_back(type);
            loadedFiles.push_back(juce::File{});
            arrangementTracks.push_back({});
            targetIdx = numActiveTracks.fetch_add(1, std::memory_order_release);
            sessionView.addTrack(TrackType::Audio, "Track " + juce::String(targetIdx + 1));
            sessionView.gridContent.columns[targetIdx]->header.hasInstrument = true;
            sessionView.gridContent.columns[targetIdx]->header.instrumentName = type;
            sessionView.gridContent.columns[targetIdx]->header.repaint();
            
            if (auto* current = audioTracks[targetIdx]->activeInstrument.load(std::memory_order_acquire)) {
                current->closeUI();
            }
            
            // Instantiate modular instrument
            auto newInst = InstrumentFactory::create(type);
            if (newInst) {
                newInst->prepareToPlay(currentSampleRate);
                audioTracks[targetIdx]->automationRegistry.clear(); // Prevent dangling pointers
                newInst->registerAutomationParameters(audioTracks[targetIdx].get());
                if (auto* old = audioTracks[targetIdx]->activeInstrument.exchange(newInst.release(), std::memory_order_acq_rel)) {
                    audioTracks[targetIdx]->instrumentGarbageQueue.push(old);
                }
                audioTracks[targetIdx]->refreshAutomationRegistry();
            }
        }

        // Always select the track and show its device panel immediately,
        // regardless of what was previously selected.
        selectedTrackIndex = targetIdx;
        selectedSceneIndex = -1;
        sessionView.setTrackSelected(targetIdx);
        sessionView.setClipSelected(targetIdx, -1);

        showDeviceEditorForTrack(targetIdx);

        resized();
        markDirty();
    };

    sessionView.onEffectDropped = [this](int trackIdx, const juce::String& type) {
        int nTracks = numActiveTracks.load(std::memory_order_relaxed);
        Track* targetTrack = nullptr;

        if (trackIdx == 999) {
            targetTrack = &returnTrackA;
        } else if (trackIdx >= 0 && trackIdx < nTracks) {
            targetTrack = audioTracks[trackIdx].get();
        } else {
            return; // Dropped on an invalid place
        }

        auto newEffect = EffectFactory::create(type);
        if (newEffect) {
            newEffect->prepareToPlay(currentSampleRate);
            newEffect->registerAutomationParameters(targetTrack);

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
        markDirty();
    };

    // ── Mixer Volume Callbacks ────────────────────────────────────────
    // These are the ONLY paths that write to the audio-thread gain atomics.
    // Without them the faders are purely cosmetic.
    sessionView.onTrackVolumeChanged = [this](int t, float gain) {
        if (t >= 0 && t < (int)audioTracks.size())
            audioTracks[t]->gain.store(gain, std::memory_order_relaxed);
    };

    sessionView.onTrackSendChanged = [this](int t, float level) {
        if (t >= 0 && t < (int)audioTracks.size())
            audioTracks[t]->sendALevel.store(level, std::memory_order_relaxed);
    };

    sessionView.onMasterVolumeChanged = [this](float gain) {
        masterTrack.gain.store(gain, std::memory_order_relaxed);
    };

    sessionView.onReturnVolumeChanged = [this](float gain) {
        returnTrackA.gain.store(gain, std::memory_order_relaxed);
    };

    sessionView.onTrackMuteChanged = [this](int t, bool muted) {
        if (t >= 0 && t < (int)audioTracks.size())
            audioTracks[t]->muted.store(muted, std::memory_order_relaxed);
    };

    sessionView.onTrackSoloChanged = [this](int t, bool soloed) {
        if (t >= 0 && t < (int)audioTracks.size())
            audioTracks[t]->soloed.store(soloed, std::memory_order_relaxed);
    };

    // ── Rename / Color callbacks ──────────────────────────────────────────────
    sessionView.onRenameClip = [this](int t, int s, const juce::String& name) {
        if (t >= 0 && t < (int)clipGrid.size() && juce::isPositiveAndBelow(s, NUM_SCENES)) {
            clipGrid[t][s].name = name;
            sessionView.setClipData(t, s, clipGrid[t][s]);
            markDirty();
        }
    };

    sessionView.onSetClipColour = [this](int t, int s, juce::Colour c) {
        if (t >= 0 && t < (int)clipGrid.size() && juce::isPositiveAndBelow(s, NUM_SCENES)) {
            clipGrid[t][s].colour = c;
            sessionView.setClipData(t, s, clipGrid[t][s]);
            markDirty();
        }
    };

    sessionView.onRenameTrack = [this](int t, const juce::String& name) {
        // The TrackHeader already updated its own trackName and repainted;
        // nothing else to do at runtime (persistence happens on Save).
        markDirty();
        juce::ignoreUnused(t, name);
    };

    sessionView.onSceneLabelClicked = [this] {
        if (topBar)
        {
            if (topBar->onSwitchToArrangement)
                topBar->onSwitchToArrangement();
        }
    };

    sessionView.onSetTrackColour = [this](int t, juce::Colour c) {
        // The TrackHeader already updated its own trackColour and repainted.
        juce::ignoreUnused(t, c);
    };

    arrangementView.onTrackSelected = [this](int trackIdx) {
        if (trackIdx < 0 || trackIdx >= (int)audioTracks.size()) return;
        selectedTrackIndex = trackIdx;
        arrangementView.setSelectedTrack(trackIdx);
        showDeviceEditorForTrack(trackIdx);
        resized(); // re-layout bottom panel if device view changed height
    };

    arrangementView.onClipSelected = [this](ArrangementClip* clip) {
        if (clip == nullptr) return;
        selectedArrangementClip = clip;
        int trackIdx = clip->trackIndex;
        if (trackIdx < 0 || trackIdx >= (int)audioTracks.size()) return;
        selectedTrackIndex = trackIdx;
        arrangementView.setSelectedTrack(trackIdx);
        // Load device editor so the user can right-click its knobs
        showDeviceEditorForTrack(trackIdx);
        resized();
    };

    arrangementView.onClipPlaced = [this](int trackIdx, double startBar, const ClipData& newClip) {
        if (trackIdx >= 0 && trackIdx < (int)arrangementTracks.size()) {
            ArrangementClip ac;
            ac.trackIndex = trackIdx;
            ac.startBar = startBar;
            ac.lengthBars = newClip.patternLengthBars > 0.0 ? newClip.patternLengthBars : 1.0;
            ac.data = newClip;
            
            if (ac.data.patternMode == "euclidean") {
                regenerateEuclideanMidi(ac.data);
            } else if (ac.data.patternMode == "drumrack") {
                regenerateDrumRackMidi(ac.data);
            }
            // pianoroll clips already carry midiNotes from the piano roll editor
            
            arrangementTracks[trackIdx].push_back(ac);
            syncArrangementFromSession();
        }
    };
    
    arrangementView.onClipDeleted = [this](ArrangementClip* clip) {
        if (clip && clip->trackIndex >= 0 && clip->trackIndex < (int)arrangementTracks.size()) {
            auto& trackClips = arrangementTracks[clip->trackIndex];
            if (selectedArrangementClip == clip) {
                selectedArrangementClip = nullptr;
                arrangementView.setSelectedClip(nullptr);
            }
            trackClips.erase(std::remove_if(trackClips.begin(), trackClips.end(),
                [clip](const ArrangementClip& c) { return &c == clip; }), trackClips.end());
            syncArrangementFromSession();
        }
    };

    arrangementView.onClipMoved = [this](ArrangementClip* clip) {
        // If trackIdx changed we need to move it to a different vector.
        // For Phase 2 we assume it just moves within the same track or we handle it here:
        if (clip) {
            // Check if track index changed
            bool found = false;
            auto& oldTrackClips = arrangementTracks[clip->trackIndex];
            for (auto it = oldTrackClips.begin(); it != oldTrackClips.end(); ++it) {
                if (&(*it) == clip) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                ArrangementClip copy = *clip;
                for (int t = 0; t < (int)arrangementTracks.size(); ++t) {
                    auto& tc = arrangementTracks[t];
                    auto it = std::remove_if(tc.begin(), tc.end(), [clip](const ArrangementClip& c) { return &c == clip; });
                    if (it != tc.end()) {
                        tc.erase(it, tc.end());
                        break;
                    }
                }
                if (copy.trackIndex >= 0 && copy.trackIndex < (int)arrangementTracks.size())
                    arrangementTracks[copy.trackIndex].push_back(copy);
            }
            syncArrangementFromSession();
        }
    };

    // When points are edited on the arrangement automation overlay, re-push
    // a new SharedArrangement so the render thread evaluates the updated lanes.
    arrangementView.automationOverlay.onAutomationChanged = [this]() {
        syncArrangementFromSession();
    };

    // ── Timeline seek: left-click on the ruler ────────────────────────────────
    // The render thread will re-anchor its renderTransportOffset via pendingSeekSample.
    arrangementView.onPlayheadScrubbed = [this](double bar) {
        double spb = transportClock.getSamplesPerBeat();
        if (spb <= 0.0) return;
        // Convert bar (1-based) → sample
        int64_t seekSample = static_cast<int64_t>((bar - 1.0) * 4.0 * spb);
        if (seekSample < 0) seekSample = 0;
        // Update the global transport position (read by timerCallback for UI)
        transportClock.seekTo(seekSample);
        // Tell the render thread to re-anchor on the next block
        pendingSeekSample.store(seekSample, std::memory_order_release);
        // Flush any currently held notes (message-thread side, for Session mode)
        const int n = numActiveTracks.load(std::memory_order_relaxed);
        for (int t = 0; t < n; ++t)
            audioTracks[t]->commandQueue.push({ TrackCommand::Type::FlushNotes, nullptr, -1.0 });
    };

    // ── Loop region: drag on the ruler ────────────────────────────────────────
    // loopIn/loopOut == -1 means the loop was cleared/disabled.
    arrangementView.onLoopChanged = [this](double loopIn, double loopOut) {
        if (loopIn < 0.0 || loopOut < 0.0 || loopIn >= loopOut) {
            transportClock.clearLoop();
        } else {
            double spb = transportClock.getSamplesPerBeat();
            if (spb > 0.0) {
                int64_t startSmp = static_cast<int64_t>((loopIn  - 1.0) * 4.0 * spb);
                int64_t endSmp   = static_cast<int64_t>((loopOut - 1.0) * 4.0 * spb);
                if (startSmp < 0) startSmp = 0;
                if (endSmp <= startSmp) endSmp = startSmp + 1;
                transportClock.setLoop(startSmp, endSmp);
            }
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

    setWantsKeyboardFocus(true);
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
        
    for (int t = 0; t < (int)audioTracks.size(); ++t) {
        if (auto* inst = audioTracks[t]->activeInstrument.load(std::memory_order_acquire)) {
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
    if (isExporting.load(std::memory_order_acquire)) {
        bufferToFill.clearActiveBufferRegion();
        return;
    }

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

    constexpr int kResizerW = 5;
    auto browserStrip = bounds.removeFromLeft(browserWidth + kResizerW);
    browser.setBounds(browserStrip.removeFromLeft(browserWidth));
    browserResizer.setBounds(browserStrip); // remaining kResizerW px

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

    if (currentView == DAWView::Session)
    {
        sessionView.setVisible(true);
        arrangementView.setVisible(false);
        sessionView.setBounds(bounds);
    }
    else
    {
        sessionView.setVisible(false);
        arrangementView.setVisible(true);
        arrangementView.setBounds(bounds);

        // In Arrangement mode, the pattern editor is replaced by the arrangement
        // automation overlay on the track row — hide it here so it doesn’t take
        // up the bottom panel space.
        patternEditor.setVisible(false);
        deviceView.setBounds(bottomPanel);
    }
}

bool MainComponent::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::tabKey)
    {
        if (currentView == DAWView::Session)
        {
            if (topBar && topBar->onSwitchToArrangement) topBar->onSwitchToArrangement();
        }
        else
        {
            if (topBar && topBar->onSwitchToSession) topBar->onSwitchToSession();
        }
        return true;
    }
    return false;
}

void MainComponent::switchToView(DAWView v)
{
    bool viewChanged = (currentView != v);
    currentView = v;
    renderIsArrangementMode.store(v == DAWView::Arrangement, std::memory_order_release);
    if (v == DAWView::Arrangement) {
        syncArrangementFromSession(); // Always re-sync when entering Arrangement
        // Re-show overlay for previously focused parameter if any
        if (!activeAutomationParameterId.isEmpty()
            && activeAutomationTrackIdx >= 0
            && activeAutomationTrackIdx < (int)arrangementTracks.size()) {
            arrangementView.setArrangementAutomation(
                activeAutomationTrackIdx,
                &arrangementTracks[activeAutomationTrackIdx],
                activeAutomationParameterId);
        }
    } else {
        // Switching back to Session — clear the arrangement automation overlay and selection
        arrangementView.clearArrangementAutomation();
        selectedArrangementClip = nullptr;
        arrangementView.setSelectedClip(nullptr);
    }
    if (viewChanged)
        resized();
}

void MainComponent::syncArrangementFromSession()
{
    std::vector<ArrangementView::TrackState> tStates;
    int nTracks = numActiveTracks.load(std::memory_order_relaxed);
    
    for (int t = 0; t < nTracks; ++t) {
        ArrangementView::TrackState state;
        state.name = "Track " + juce::String(t + 1);
        
        // Grab track name and color from SessionView's columns if available
        if (t < sessionView.gridContent.columns.size()) {
            if (auto* col = sessionView.gridContent.columns[t]) {
                state.name = col->header.trackName;
                state.colour = col->header.trackColour;
            }
        }
        
        // Gather clips present on this track
        for (int s = 0; s < NUM_SCENES; ++s) {
            if (clipGrid[t][s].hasClip) {
                state.availableClips.push_back(clipGrid[t][s]);
            }
        }
        
        tStates.push_back(state);
    }
    
    arrangementView.setTracksAndClips(tStates, arrangementTracks.data());

    // ── Update Audio Engine lock-free timeline ─────────────────────────────
    auto* newArrangement = new SharedArrangement();
    newArrangement->tracks.resize(nTracks);
    for (int t = 0; t < nTracks; ++t) {
        newArrangement->tracks[t] = arrangementTracks[t];
    }
    
    auto* oldArrangement = renderArrangement.exchange(newArrangement, std::memory_order_acq_rel);
    if (oldArrangement) {
        arrangementGarbageQueue.push(oldArrangement);
    }
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
            peakTrackRms = std::max(peakTrackRms, audioTracks[t]->rmsLevel.load(std::memory_order_relaxed));
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
    for (int t = 0; t < nTracks; ++t) gcTrack(*audioTracks[t]);

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
        // Drain replaced instruments (e.g. PluginInstrumentAdapter with open windows).
        // NOTE: instrumentGarbageQueue is drained on the RENDER thread (see RenderThread::run)
        // to avoid use-after-free. Do NOT drain it here.
        while (auto opt = t.effectGarbageQueue.pop()) {
            delete *opt;
        }
    };
    
    for (int t = 0; t < nTracks; ++t) {
        gcProcessors(*audioTracks[t]);
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
        if (playing && audioTracks[t]->currentPattern && spb > 0.0) {
            double lenBeats = audioTracks[t]->currentPattern->getLengthBeats();
            double lenSamples = lenBeats * spb;
            if (lenSamples > 0.0) {
                double elapsed = static_cast<double>(playhead - audioTracks[t]->patternStartSample);
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
            float newRms  = audioTracks[t]->rmsLevel.load (std::memory_order_relaxed);
            float newGain = audioTracks[t]->gain.load     (std::memory_order_relaxed);
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

    // ── Update ArrangementView playhead ───────────────────────────────────────
    if (currentView == DAWView::Arrangement) {
        double currentBar = 1.0;
        if (spb > 0.0) {
            double currentBeats = static_cast<double>(playhead) / spb;
            currentBar = 1.0 + (currentBeats / 4.0); // Assuming 4/4 time for now
        }
        arrangementView.setPlayheadBar(currentBar);
    }
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

    // ── Restore plugin search paths & run initial scan ────────────────────────
    if (userSettings != nullptr)
    {
        juce::String savedPaths = userSettings->getValue ("pluginSearchPaths");
        if (savedPaths.isNotEmpty())
        {
            juce::StringArray paths;
            paths.addTokens (savedPaths, ";", "");
            browser.setPluginSearchPaths (paths);
        }
    }
    // Always ensure the downloaded Vital path is in the list
    {
        juce::File vitalDefault (u8"/home/vietnoirien/Téléchargements/VitalInstaller/lib");
        if (vitalDefault.isDirectory())
            browser.addPluginSearchPath (vitalDefault);
    }
    browser.rescanPlugins (pluginFormatManager);

    DBG("Device settings loaded successfully.");
}

// ════════════════════════════════════════════════════════════════════════════
//  Project Management & Workspace Lifecycle
// ════════════════════════════════════════════════════════════════════════════

void MainComponent::updateWindowTitle()
{
    juce::String projectName = (currentProjectFile != juce::File{})
                                   ? currentProjectFile.getFileNameWithoutExtension()
                                   : "Untitled";
    juce::String title = "LiBeDAW \u2014 " + projectName + (projectIsDirty ? " *" : "");

    if (auto* tlc = getTopLevelComponent())
        tlc->setName(title);
}

void MainComponent::markDirty()
{
    if (!projectIsDirty)
    {
        projectIsDirty = true;
        updateWindowTitle();
    }
}

bool MainComponent::saveIfNeededBeforeQuit()
{
    if (!projectIsDirty)
        return true; // nothing unsaved — proceed with quit

    juce::String projectName = (currentProjectFile != juce::File{})
                                   ? currentProjectFile.getFileNameWithoutExtension()
                                   : "Untitled";

    const int result = juce::NativeMessageBox::showYesNoCancelBox(
        juce::MessageBoxIconType::QuestionIcon,
        "Unsaved Changes",
        "\"" + projectName + "\" has unsaved changes.\nDo you want to save before quitting?",
        nullptr, nullptr);

    // result: 1 = Yes (Save), 0 = No (Discard), -1 = Cancel
    if (result == 1)
    {
        // Save now, then quit
        if (currentProjectFile == juce::File{})
        {
            // No file yet — trigger Save As synchronously via the existing callback
            if (topBar && topBar->onSaveProjectAs)
                topBar->onSaveProjectAs();
            // After Save As the dirty flag will be cleared by the lambda;
            // we still allow quit (the file chooser already ran).
        }
        else
        {
            syncProjectToUI();
            projectManager.saveProject(currentProjectFile);
            projectIsDirty = false;
            updateWindowTitle();
        }
        return true;
    }
    else if (result == 0)
    {
        return true; // Discard — proceed with quit
    }
    else
    {
        return false; // Cancel — abort quit
    }
}


bool MainComponent::ensureWorkspaceDirectory()
{
    juce::File appDataDir = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("LiBeDAW");
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
                    workspaceDirectory = juce::File::getSpecialLocation(juce::File::userHomeDirectory).getChildFile("LiBeDAW_Projects");
                }
                
                workspaceDirectory.createDirectory();
                juce::File appDataDir = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("LiBeDAW");
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

    juce::File settingsFile = workspaceDirectory.getChildFile("Settings").getChildFile("LiBeDAW.settings");
    
    juce::PropertiesFile::Options opts;
    opts.applicationName = "LiBeDAW";
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
        projectIsDirty = false;
        syncUIToProject();
        updateWindowTitle();
    };

    topBar->onOpenProject = [this] {
        projectChooser = std::make_unique<juce::FileChooser>("Open Project...", workspaceDirectory.getChildFile("Projects"), "*.LBD");
        projectChooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
            [this](const juce::FileChooser& fc) {
                auto result = fc.getResult();
                if (result != juce::File{}) {
                    transportClock.stop();
                    if (projectManager.loadProject(result)) {
                        currentProjectFile = result;
                        projectIsDirty = false;
                        syncUIToProject();
                        updateWindowTitle();
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
            projectIsDirty = false;
            updateWindowTitle();
        }
    };

    topBar->onSaveProjectAs = [this] {
        projectChooser = std::make_unique<juce::FileChooser>("Save Project As...", workspaceDirectory.getChildFile("Projects"), "*.LBD");
        projectChooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles,
            [this](const juce::FileChooser& fc) {
                auto result = fc.getResult();
                if (result != juce::File{}) {
                    if (!result.hasFileExtension(".LBD")) {
                        result = result.withFileExtension(".LBD");
                    }
                    currentProjectFile = result;
                    syncProjectToUI();
                    projectManager.saveProject(currentProjectFile);
                    projectIsDirty = false;
                    updateWindowTitle();
                }
            });
    };

    topBar->onBpmChanged = [this] { markDirty(); };
}

void MainComponent::syncUIToProject()
{
    // Clear current tracks and UI
    numActiveTracks.store(0, std::memory_order_relaxed);
    // Clear all existing tracks (instruments, effects, commands)
    for (int t = 0; t < (int)audioTracks.size(); ++t) {
        audioTracks[t]->gain.store(1.0f);
        audioTracks[t]->commandQueue.push({ TrackCommand::Type::StopPattern, nullptr, 0 });
        for (int e = 0; e < Track::MAX_EFFECTS; ++e) {
            if (auto* old = audioTracks[t]->effectChain[e].exchange(nullptr, std::memory_order_acq_rel)) {
                audioTracks[t]->effectGarbageQueue.push(old);
            }
        }
    }
    // Wipe all vectors (clips are UI-thread-only, safe here after stop)
    audioTracks.clear();
    clipGrid.clear();
    trackInstruments.clear();
    loadedFiles.clear();
    arrangementTracks.clear();
    // Re-reserve so push_back stays safe
    audioTracks.reserve(128);
    clipGrid.reserve(128);
    trackInstruments.reserve(128);
    loadedFiles.reserve(128);
    arrangementTracks.reserve(128);
    
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
                if (tIdx >= 0)
                {
                    // Grow all parallel vectors to accommodate this track index
                    while ((int)audioTracks.size() <= tIdx) {
                        audioTracks.push_back(std::make_unique<Track>());
                        clipGrid.push_back({});
                        trackInstruments.push_back("");
                        loadedFiles.push_back(juce::File{});
                        arrangementTracks.push_back({});
                    }
                    numActiveTracks.store(std::max(numActiveTracks.load(), tIdx + 1), std::memory_order_relaxed);
                    
                    juce::String name = trackNode.getProperty("name", "Track " + juce::String(tIdx + 1));
                    float gain = trackNode.getProperty("gain", 1.0f);
                    audioTracks[tIdx]->gain.store(gain);

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
                            
                            if (auto* old = audioTracks[tIdx]->activeInstrument.exchange(newInst.release(), std::memory_order_acq_rel)) {
                                audioTracks[tIdx]->instrumentGarbageQueue.push(old);
                            }

                            auto* currentInst = audioTracks[tIdx]->activeInstrument.load(std::memory_order_acquire);

                            if (instrumentType == "Simpler") {
                                auto samplerNode = trackNode.getChildWithName("Sampler");
                                if (samplerNode.isValid()) {
                                    juce::String filePath = samplerNode.getProperty("file_path", "");
                                    juce::File f(filePath);
                                    if (f.existsAsFile()) {
                                        currentInst->loadFile(f);
                                        loadAudioFileIntoTrack(tIdx, f);
                                    }
                                }
                            } else {
                                auto stateNode = trackNode.getChildWithName(instrumentType + "State");
                                if (stateNode.isValid()) {
                                    currentInst->loadState(stateNode);
                                    if (instrumentType == "DrumRack") {
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
                                    if (auto* old = audioTracks[tIdx]->effectChain[eIdx].exchange(effect.release(), std::memory_order_acq_rel)) {
                                        audioTracks[tIdx]->effectGarbageQueue.push(old);
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
                                    d.euclideanBars   = clipNode.getProperty("euclideanBars",   1);
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

                                    // Automation lanes
                                    auto lanesNode = clipNode.getChildWithName("AutomationLanes");
                                    if (lanesNode.isValid()) {
                                        for (int li = 0; li < lanesNode.getNumChildren(); ++li) {
                                            auto laneNode = lanesNode.getChild(li);
                                            AutomationLane lane;
                                            lane.parameterId = laneNode.getProperty("parameterId", "");
                                            if (lane.parameterId.isEmpty()) continue;
                                            for (int pi = 0; pi < laneNode.getNumChildren(); ++pi) {
                                                auto ptNode = laneNode.getChild(pi);
                                                AutomationPoint pt;
                                                pt.positionBeats = ptNode.getProperty("pos",   0.0);
                                                pt.value         = ptNode.getProperty("value", 0.5f);
                                                lane.points.push_back(pt);
                                            }
                                            if (!lane.points.empty())
                                                d.automationLanes.push_back(std::move(lane));
                                        }
                                    }
                                    
                                    clipGrid[tIdx][sIdx] = d;
                                    // Bake MIDI so clips are arrangement-ready after project load
                                    if (clipGrid[tIdx][sIdx].patternMode == "drumrack") {
                                        regenerateDrumRackMidi(clipGrid[tIdx][sIdx]);
                                    } else if (clipGrid[tIdx][sIdx].patternMode == "euclidean") {
                                        regenerateEuclideanMidi(clipGrid[tIdx][sIdx]);
                                    }
                                    sessionView.setClipData(tIdx, sIdx, clipGrid[tIdx][sIdx]);
                                }
                            }
                        }
                    }

                    // ArrangementClips
                    auto arrNode = trackNode.getChildWithName("ArrangementClips");
                    if (arrNode.isValid()) {
                        for (int c = 0; c < arrNode.getNumChildren(); ++c) {
                            auto aNode = arrNode.getChild(c);
                            if (aNode.hasType("ArrangementClip")) {
                                ArrangementClip aClip;
                                aClip.trackIndex = tIdx;
                                aClip.startBar = aNode.getProperty("startBar", 1.0);
                                aClip.lengthBars = aNode.getProperty("lengthBars", 1.0);
                                
                                auto clipNode = aNode.getChildWithName("Clip");
                                if (clipNode.isValid()) {
                                    ClipData d;
                                    d.hasClip  = true;
                                    d.isPlaying = false;
                                    d.name     = clipNode.getProperty("name", "Pattern");
                                    auto clipArgbProp = clipNode.getProperty("colour");
                                    d.colour = clipArgbProp.isVoid() ? juce::Colour (0xff2d89ef) : juce::Colour ((juce::uint32)(juce::int64)clipArgbProp);
                                    d.euclideanSteps  = clipNode.getProperty("euclideanSteps",  16);
                                    d.euclideanPulses = clipNode.getProperty("euclideanPulses", 4);
                                    d.euclideanBars   = clipNode.getProperty("euclideanBars",   1);
                                    juce::String hex = clipNode.getProperty("hitMap", "");
                                    if (hex.isNotEmpty() && hex.length() % 2 == 0) {
                                        d.hitMap.clear();
                                        for (int ci = 0; ci < hex.length(); ci += 2) d.hitMap.push_back((uint8_t) hex.substring(ci, ci + 2).getHexValue32());
                                    }
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
                                                juce::String phex = pNode.getProperty("hitMap", "");
                                                if (phex.isNotEmpty() && phex.length() % 2 == 0) {
                                                    pad.hitMap.clear();
                                                    for (int ci = 0; ci < phex.length(); ci += 2) pad.hitMap.push_back((uint8_t)phex.substring(ci, ci + 2).getHexValue32());
                                                }
                                            }
                                        }
                                    }

                                    // Automation lanes (arrangement clip)
                                    auto lanesNode = clipNode.getChildWithName("AutomationLanes");
                                    if (lanesNode.isValid()) {
                                        for (int li = 0; li < lanesNode.getNumChildren(); ++li) {
                                            auto laneNode = lanesNode.getChild(li);
                                            AutomationLane lane;
                                            lane.parameterId = laneNode.getProperty("parameterId", "");
                                            if (lane.parameterId.isEmpty()) continue;
                                            for (int pi = 0; pi < laneNode.getNumChildren(); ++pi) {
                                                auto ptNode = laneNode.getChild(pi);
                                                AutomationPoint pt;
                                                pt.positionBeats = ptNode.getProperty("pos",   0.0);
                                                pt.value         = ptNode.getProperty("value", 0.5f);
                                                lane.points.push_back(pt);
                                            }
                                            if (!lane.points.empty())
                                                d.automationLanes.push_back(std::move(lane));
                                        }
                                    }
                                    aClip.data = d;
                                }
                                arrangementTracks[tIdx].push_back(aClip);
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

    syncArrangementFromSession();
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
        trackNode.setProperty("gain", audioTracks[t]->gain.load(std::memory_order_relaxed), nullptr);

        trackNode.setProperty("instrument_type", trackInstruments[t], nullptr);

        if (auto* inst = audioTracks[t]->activeInstrument.load(std::memory_order_acquire)) {
            if (trackInstruments[t] == "Simpler") {
                if (auto* simpler = dynamic_cast<SimplerProcessor*>(inst)) {
                    if (simpler->loadedFile.existsAsFile()) {
                        juce::ValueTree samplerNode("Sampler");
                        samplerNode.setProperty("file_path", simpler->loadedFile.getFullPathName(), nullptr);
                        trackNode.addChild(samplerNode, -1, nullptr);
                    }
                }
            } else {
                auto state = inst->saveState();
                trackNode.addChild(state, -1, nullptr);
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
                clipNode.setProperty("euclideanBars",   clip.euclideanBars,   nullptr);
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

                // Automation lanes
                if (!clip.automationLanes.empty()) {
                    juce::ValueTree lanesNode("AutomationLanes");
                    for (const auto& lane : clip.automationLanes) {
                        if (lane.points.empty()) continue;
                        juce::ValueTree laneNode("Lane");
                        laneNode.setProperty("parameterId", lane.parameterId, nullptr);
                        for (const auto& pt : lane.points) {
                            juce::ValueTree ptNode("Point");
                            ptNode.setProperty("pos",   pt.positionBeats, nullptr);
                            ptNode.setProperty("value", pt.value,         nullptr);
                            laneNode.addChild(ptNode, -1, nullptr);
                        }
                        lanesNode.addChild(laneNode, -1, nullptr);
                    }
                    if (lanesNode.getNumChildren() > 0)
                        clipNode.addChild(lanesNode, -1, nullptr);
                }
            }
        }
        if (clipsNode.getNumChildren() > 0)
            trackNode.addChild(clipsNode, -1, nullptr);
            
        // ArrangementClips
        juce::ValueTree arrangementNode("ArrangementClips");
        for (const auto& aClip : arrangementTracks[t]) {
            juce::ValueTree aClipNode("ArrangementClip");
            aClipNode.setProperty("startBar", aClip.startBar, nullptr);
            aClipNode.setProperty("lengthBars", aClip.lengthBars, nullptr);
            
            juce::ValueTree innerClipNode("Clip");
            innerClipNode.setProperty("name", aClip.data.name, nullptr);
            innerClipNode.setProperty("colour", (juce::int64)aClip.data.colour.getARGB(), nullptr);
            innerClipNode.setProperty("euclideanSteps", aClip.data.euclideanSteps, nullptr);
            innerClipNode.setProperty("euclideanPulses", aClip.data.euclideanPulses, nullptr);
            innerClipNode.setProperty("euclideanBars", aClip.data.euclideanBars, nullptr);
            if (!aClip.data.hitMap.empty()) {
                juce::String hex;
                for (uint8_t b : aClip.data.hitMap) hex += juce::String::toHexString(b).paddedLeft('0', 2);
                innerClipNode.setProperty("hitMap", hex, nullptr);
            }
            innerClipNode.setProperty("patternMode", aClip.data.patternMode, nullptr);
            innerClipNode.setProperty("patternLengthBars", aClip.data.patternLengthBars, nullptr);

            if (!aClip.data.midiNotes.empty()) {
                juce::ValueTree midiNotesNode("MidiNotes");
                for (const auto& n : aClip.data.midiNotes) {
                    juce::ValueTree noteNode("Note");
                    noteNode.setProperty("note", n.note, nullptr);
                    noteNode.setProperty("startBeat", n.startBeat, nullptr);
                    noteNode.setProperty("lengthBeats", n.lengthBeats, nullptr);
                    noteNode.setProperty("velocity", n.velocity, nullptr);
                    midiNotesNode.addChild(noteNode, -1, nullptr);
                }
                innerClipNode.addChild(midiNotesNode, -1, nullptr);
            }

            if (aClip.data.patternMode == "drumrack") {
                juce::ValueTree drNode("DrumPatterns");
                for (int p = 0; p < 16; ++p) {
                    auto& pad = aClip.data.drumPatterns[p];
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
                innerClipNode.addChild(drNode, -1, nullptr);
            }

            // Automation lanes (arrangement clip)
            if (!aClip.data.automationLanes.empty()) {
                juce::ValueTree lanesNode("AutomationLanes");
                for (const auto& lane : aClip.data.automationLanes) {
                    if (lane.points.empty()) continue;
                    juce::ValueTree laneNode("Lane");
                    laneNode.setProperty("parameterId", lane.parameterId, nullptr);
                    for (const auto& pt : lane.points) {
                        juce::ValueTree ptNode("Point");
                        ptNode.setProperty("pos",   pt.positionBeats, nullptr);
                        ptNode.setProperty("value", pt.value,         nullptr);
                        laneNode.addChild(ptNode, -1, nullptr);
                    }
                    lanesNode.addChild(laneNode, -1, nullptr);
                }
                if (lanesNode.getNumChildren() > 0)
                    innerClipNode.addChild(lanesNode, -1, nullptr);
            }
            aClipNode.addChild(innerClipNode, -1, nullptr);
            arrangementNode.addChild(aClipNode, -1, nullptr);
        }
        if (arrangementNode.getNumChildren() > 0)
            trackNode.addChild(arrangementNode, -1, nullptr);
            
        // Effects
        juce::ValueTree effectsNode("Effects");
        for (int i = 0; i < Track::MAX_EFFECTS; ++i) {
            if (auto* effect = audioTracks[t]->effectChain[i].load(std::memory_order_acquire)) {
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
    if (trackIdx < 0 || trackIdx >= (int)audioTracks.size()) return;

    if (auto* inst = audioTracks[trackIdx]->activeInstrument.load(std::memory_order_acquire)) {
        inst->loadFile(file);
    }

    juce::Thread::launch([this, file, trackIdx] {
        if (auto* reader = formatManager.createReaderFor(file)) {
            auto* newBuffer = new juce::AudioBuffer<float>(
                reader->numChannels, (int)reader->lengthInSamples);
            reader->read(newBuffer, 0, (int)reader->lengthInSamples, 0, true, true);
            delete reader;

            juce::MessageManager::callAsync([this, newBuffer, trackIdx, file] {
                if (auto* inst = audioTracks[trackIdx]->activeInstrument.load(std::memory_order_acquire)) {
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

void MainComponent::regenerateEuclideanMidi(ClipData& clip) {
    clip.midiNotes.clear();
    std::vector<uint8_t> map = clip.hitMap;
    if (map.empty() && clip.euclideanPulses > 0) {
        map.assign(clip.euclideanSteps, 0);
        for (int i = 0; i < clip.euclideanSteps; ++i) {
            map[i] = ((i * clip.euclideanPulses) % clip.euclideanSteps < clip.euclideanPulses) ? 1 : 0;
        }
    }
    
    for (int i = 0; i < clip.euclideanSteps; ++i) {
        if (map[i] != 0) {
            MidiNote n;
            n.note = 60; // Middle C for euclidean synth playback
            n.startBeat = (double)i * 0.25; // Assuming 16th notes
            n.lengthBeats = 0.25;
            n.velocity = 1.0f;
            clip.midiNotes.push_back(n);
        }
    }
}

void MainComponent::loadAudioFileIntoDrumPad(int trackIdx, int padIndex, const juce::File& file) {
    if (trackIdx < 0 || trackIdx >= (int)audioTracks.size()) return;

    if (auto* inst = audioTracks[trackIdx]->activeInstrument.load(std::memory_order_acquire)) {
        if (auto* dr = dynamic_cast<DrumRackProcessor*>(inst)) {
            juce::Thread::launch([this, file, trackIdx, padIndex] {
                if (auto* reader = formatManager.createReaderFor(file)) {
                    auto* newBuffer = new juce::AudioBuffer<float>(
                        reader->numChannels, (int)reader->lengthInSamples);
                    reader->read(newBuffer, 0, (int)reader->lengthInSamples, 0, true, true);
                    delete reader;

                    juce::MessageManager::callAsync([this, newBuffer, trackIdx, padIndex, file] {
                        if (auto* inst = audioTracks[trackIdx]->activeInstrument.load(std::memory_order_acquire)) {
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
    } else if (trackIdx >= 0 && trackIdx < (int)audioTracks.size()) {
        track = audioTracks[trackIdx].get();
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
                track->refreshAutomationRegistry();
                showDeviceEditorForTrack(trackIdx);
            };
            deviceView.addEditor(std::move(wrapper));
        }
    }

    std::unordered_map<juce::String, DeviceView::AutomationParamInfo> autoInfo;
    if (trackIdx >= 0 && trackIdx < (int)audioTracks.size() && selectedSceneIndex >= 0 && selectedSceneIndex < NUM_SCENES) {
        auto& clip = clipGrid[trackIdx][selectedSceneIndex];
        auto* track = audioTracks[trackIdx].get();
        for (const auto& lane : clip.automationLanes) {
            if (lane.points.empty()) continue;
            float minV = lane.points[0].value, maxV = lane.points[0].value;
            for (const auto& pt : lane.points) {
                minV = std::min(minV, pt.value);
                maxV = std::max(maxV, pt.value);
            }
            // minV/maxV are already normalized [0,1] — they are the raw normalised values stored in points
            autoInfo[lane.parameterId] = { true, minV, maxV };
        }
    }
    deviceView.updateAutomationIndicators(autoInfo);
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

// ── loadPluginAsTrackInstrument ───────────────────────────────────────────────
// Loads a VST3/LV2 plugin file and installs it as the InstrumentProcessor on
// the target track.  If trackIdx is -1 (dropped on the drop zone), a new track
// is created first.
void MainComponent::loadPluginAsTrackInstrument (int trackIdx, const juce::File& pluginFile)
{
    // ── Scan the plugin file for descriptions ──────────────────────────────
    juce::String errorMessage;
    juce::OwnedArray<juce::PluginDescription> foundDescs;

    for (auto* fmt : pluginFormatManager.getFormats())
    {
        if (!fmt->fileMightContainThisPluginType (pluginFile.getFullPathName())) continue;
        fmt->findAllTypesForFile (foundDescs, pluginFile.getFullPathName());
        if (!foundDescs.isEmpty()) break;
    }

    if (foundDescs.isEmpty())
    {
        DBG ("loadPluginAsTrackInstrument: no plugin found in " + pluginFile.getFullPathName());
        return;
    }

    // ── Create the instance ────────────────────────────────────────────────
    std::unique_ptr<juce::AudioPluginInstance> instance;
    for (auto* fmt : pluginFormatManager.getFormats())
    {
        if (!fmt->fileMightContainThisPluginType (pluginFile.getFullPathName())) continue;
        instance = fmt->createInstanceFromDescription (*foundDescs[0],
                                                        currentSampleRate,
                                                        currentBufferSize,
                                                        errorMessage);
        if (instance) break;
    }

    if (!instance)
    {
        DBG ("loadPluginAsTrackInstrument: instantiation failed — " + errorMessage);
        return;
    }

    instance->prepareToPlay (currentSampleRate, currentBufferSize);

    // ── Determine / create the target track ───────────────────────────────
    int nTracks   = numActiveTracks.load (std::memory_order_relaxed);
    int targetIdx = trackIdx;

    if (targetIdx < 0 || targetIdx >= nTracks)
    {
        // Dropped on the drop zone — create a new track
        juce::String pluginName = foundDescs[0]->name;
        audioTracks.push_back (std::make_unique<Track>());
        clipGrid.push_back ({});
        trackInstruments.push_back ("Plugin:" + pluginName);
        loadedFiles.push_back (juce::File{});
        arrangementTracks.push_back ({});
        targetIdx = numActiveTracks.fetch_add (1, std::memory_order_release);
        sessionView.addTrack (TrackType::Audio, pluginName);
        sessionView.gridContent.columns[targetIdx]->header.hasInstrument = true;
        sessionView.gridContent.columns[targetIdx]->header.instrumentName = "Plugin:" + pluginName;
        sessionView.gridContent.columns[targetIdx]->header.repaint();
    }
    else
    {
        juce::String pluginName = foundDescs[0]->name;
        trackInstruments[targetIdx] = "Plugin:" + pluginName;
        sessionView.gridContent.columns[targetIdx]->header.hasInstrument = true;
        sessionView.gridContent.columns[targetIdx]->header.instrumentName = "Plugin:" + pluginName;
        sessionView.gridContent.columns[targetIdx]->header.repaint();
    }

    if (auto* current = audioTracks[targetIdx]->activeInstrument.load(std::memory_order_acquire)) {
        current->closeUI();
    }

    // ── Install as InstrumentProcessor ────────────────────────────────────
    auto* adapter = new PluginInstrumentAdapter (std::move (instance), foundDescs[0]->name);
    if (auto* old = audioTracks[targetIdx]->activeInstrument.exchange (
            adapter, std::memory_order_acq_rel))
        audioTracks[targetIdx]->instrumentGarbageQueue.push (old);
    audioTracks[targetIdx]->refreshAutomationRegistry();

    // ── Select track and refresh UI ───────────────────────────────────────
    selectedTrackIndex = targetIdx;
    selectedSceneIndex = -1;
    sessionView.setTrackSelected (targetIdx);
    sessionView.setClipSelected  (targetIdx, -1);
    showDeviceEditorForTrack (targetIdx);
    resized();
}

void MainComponent::exportProject(const juce::File& outputFile, const juce::String& format)
{
    double endBar = 1.0;
    for (int t = 0; t < numActiveTracks.load(std::memory_order_relaxed); ++t) {
        for (const auto& aClip : arrangementTracks[t]) {
            double clipEnd = aClip.startBar + aClip.lengthBars;
            if (clipEnd > endBar) endBar = clipEnd;
        }
    }
    
    // Add a 1-bar tail
    endBar += 1.0;
    
    double samplesPerBeat = transportClock.getSamplesPerBeat();
    double beatsPerBar = 4.0;
    int totalSamplesToRender = static_cast<int>((endBar - 1.0) * beatsPerBar * samplesPerBeat);
    
    if (totalSamplesToRender <= 0) {
        juce::MessageManager::callAsync([]() {
            juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon, "Export Failed", "The project is empty.");
        });
        return;
    }

    // 1. Temporarily stop the audio engine from pulling blocks
    isExporting.store(true, std::memory_order_release);
    
    // 2. Prepare temp file
    juce::File tempWav = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("libedaw_export_temp.wav");
    tempWav.deleteFile();
    
    std::unique_ptr<juce::FileOutputStream> outStream(tempWav.createOutputStream());
    if (!outStream) {
        isExporting.store(false, std::memory_order_release);
        juce::MessageManager::callAsync([]() {
            juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon, "Export Failed", "Could not create temporary file.");
        });
        return;
    }
    
    juce::WavAudioFormat wavFormat;
    const auto writerOpts = juce::AudioFormatWriterOptions{}
                                .withSampleRate        (currentSampleRate)
                                .withNumChannels       (2)
                                .withBitsPerSample     (16)
                                .withQualityOptionIndex(0);
    auto ownedStream = std::unique_ptr<juce::OutputStream>(outStream.release());
    std::unique_ptr<juce::AudioFormatWriter> writer(wavFormat.createWriterFor(ownedStream, writerOpts));
    if (!writer) {
        isExporting.store(false, std::memory_order_release);
        juce::MessageManager::callAsync([]() {
            juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon, "Export Failed", "Could not create audio writer.");
        });
        return;
    }
    // ownedStream ownership transferred to writer above
    
    // 3. Reset internal ring buffer and transport
    transportClock.stop();
    appBuffer.clear();
    renderTransportOffset.store(0, std::memory_order_release);

    transportClock.play();
    
    renderIsArrangementMode.store(true, std::memory_order_release);
    juce::MessageManager::callAsync([this]() { syncArrangementFromSession(); });
    // Wait briefly for UI message thread to sync the arrangement struct to atomic
    juce::Thread::sleep(100);

    // 4. Drain blocks as the RenderThread fills them
    juce::AudioBuffer<float> exportBuffer(2, currentBufferSize);
    int samplesRendered = 0;
    
    while (samplesRendered < totalSamplesToRender) {
        if (juce::Thread::getCurrentThread() && juce::Thread::getCurrentThread()->threadShouldExit()) break;

        // Manually wake the render thread to ensure it fills the buffer as fast as possible
        renderWakeEvent.signal();

        int ready = appBuffer.getNumReady();
        if (ready >= currentBufferSize) {
            exportBuffer.setSize(2, currentBufferSize, false, false, true);
            appBuffer.readBlock(exportBuffer, currentBufferSize);
            
            int samplesToWrite = std::min(currentBufferSize, totalSamplesToRender - samplesRendered);
            writer->writeFromAudioSampleBuffer(exportBuffer, 0, samplesToWrite);
            samplesRendered += samplesToWrite;
        } else {
            juce::Thread::sleep(1);
        }
    }
    
    // 5. Cleanup and restore state
    transportClock.stop();
    writer.reset();
    
    isExporting.store(false, std::memory_order_release);
    
    // 6. Convert if necessary
    if (format == "WAV") {
        outputFile.deleteFile();
        tempWav.moveFileTo(outputFile);
    } else {
        juce::ChildProcess ffmpeg;
        juce::StringArray args;
        args.add("ffmpeg");
        args.add("-y");
        args.add("-i");
        args.add(tempWav.getFullPathName());
        args.add(outputFile.getFullPathName());
        
        if (ffmpeg.start(args)) {
            ffmpeg.waitForProcessToFinish(30000);
        }
        tempWav.deleteFile();
    }
    
    juce::MessageManager::callAsync([]() {
        juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::InfoIcon, "Export Complete", "Audio exported successfully.");
    });
}
