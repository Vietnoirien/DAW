#include "MainComponent.h"
#include "../Effects/BespokeEffectEditors.h"

// ════════════════════════════════════════════════════════════════════════════
//  RecordingThread::run()
// ════════════════════════════════════════════════════════════════════════════
void RecordingThread::run()
{
    // Track the last observed loopWrapCounter so we detect new wraps.
    int lastLoopWrap = owner.loopWrapCounter.load(std::memory_order_relaxed);

    while (!threadShouldExit())
    {
        bool isRecording = owner.transportClock.getIsRecording();
        const int nTracks = owner.numActiveTracks.load(std::memory_order_relaxed);

        // ── Detect loop-wrap (render thread increments loopWrapCounter) ────────
        const int currentWrap = owner.loopWrapCounter.load(std::memory_order_acquire);
        const bool loopWrapped = (currentWrap != lastLoopWrap);
        lastLoopWrap = currentWrap;

        for (int t = 0; t < nTracks; ++t) {
            auto* track = owner.audioTracks[t].get();
            bool isArmed = track->isArmedForRecord.load(std::memory_order_relaxed);

            if (isRecording && isArmed) {
                // ── Loop-wrap during recording: finalise current take and start new one ──
                if (loopWrapped && track->wasRecording && track->recordWriter != nullptr) {
                    // Drain remaining FIFO to disk before sealing
                    if (track->recordFifo != nullptr) {
                        int s1, sz1, s2, sz2;
                        track->recordFifo->prepareToRead(track->recordFifo->getNumReady(), s1, sz1, s2, sz2);
                        if (sz1 > 0) { const float* d = track->recordBuffer.data() + s1; track->recordWriter->writeFromFloatArrays(&d, 1, sz1); }
                        if (sz2 > 0) { const float* d = track->recordBuffer.data() + s2; track->recordWriter->writeFromFloatArrays(&d, 1, sz2); }
                        track->recordFifo->finishedRead(sz1 + sz2);
                    }
                    // Seal current WAV
                    track->recordWriter.reset();

                    // Dispatch take registration to message thread before opening new file
                    juce::File finishedFile = track->currentRecordingFile;
                    int64_t    startSample  = track->recordStartSample;
                    juce::MessageManager::callAsync([&owner = this->owner, t, finishedFile, startSample]() {
                        owner.registerNewTake(t, finishedFile, startSample);
                    });

                    // Open a new WAV for the next take immediately
                    track->currentRecordingFile = owner.workspaceDirectory
                        .getChildFile("Audio")
                        .getChildFile("Recording_Track" + juce::String(t + 1)
                                      + "_" + juce::String(juce::Time::currentTimeMillis()) + ".wav");
                    track->currentRecordingFile.getParentDirectory().createDirectory();
                    auto outStream = track->currentRecordingFile.createOutputStream();
                    if (outStream != nullptr) {
                        juce::WavAudioFormat wavFormat;
                        track->recordWriter.reset(wavFormat.createWriterFor(
                            outStream.release(), owner.currentSampleRate, 1, 24, {}, 0));
                    }
                    track->recordStartSample = owner.transportClock.getPlayheadPosition();
                    // wasRecording stays true — we're still recording
                }
                else if (!track->wasRecording) {
                    // ── First-time init for this pass ──────────────────────────────────
                    track->currentRecordingFile = owner.workspaceDirectory
                        .getChildFile("Audio")
                        .getChildFile("Recording_Track" + juce::String(t + 1)
                                      + "_" + juce::String(juce::Time::currentTimeMillis()) + ".wav");
                    track->currentRecordingFile.getParentDirectory().createDirectory();

                    auto outStream = track->currentRecordingFile.createOutputStream();
                    if (outStream != nullptr) {
                        juce::WavAudioFormat wavFormat;
                        track->recordWriter.reset(wavFormat.createWriterFor(
                            outStream.release(), owner.currentSampleRate, 1, 24, {}, 0));
                    }
                    track->recordStartSample = owner.transportClock.getPlayheadPosition();
                    track->wasRecording = true;
                }

                // Drain FIFO to disk
                if (track->recordFifo != nullptr && track->recordWriter != nullptr) {
                    int start1, size1, start2, size2;
                    track->recordFifo->prepareToRead(track->recordFifo->getNumReady(), start1, size1, start2, size2);
                    if (size1 > 0) { const float* data = track->recordBuffer.data() + start1; track->recordWriter->writeFromFloatArrays(&data, 1, size1); }
                    if (size2 > 0) { const float* data = track->recordBuffer.data() + start2; track->recordWriter->writeFromFloatArrays(&data, 1, size2); }
                    track->recordFifo->finishedRead(size1 + size2);
                }
            } else {
                // ── Stop recording ─────────────────────────────────────────────────
                if (track->wasRecording) {
                    track->recordWriter.reset();
                    track->wasRecording = false;

                    // Dispatch take registration and comp player reload to message thread
                    juce::File finishedFile = track->currentRecordingFile;
                    int64_t    startSample  = track->recordStartSample;
                    juce::MessageManager::callAsync([&owner = this->owner, t, finishedFile, startSample]() {
                        owner.registerNewTake(t, finishedFile, startSample);
                    });
                }
                // Flush any remaining FIFO data so it doesn't build up
                if (track->recordFifo != nullptr) {
                    int start1, size1, start2, size2;
                    track->recordFifo->prepareToRead(track->recordFifo->getNumReady(), start1, size1, start2, size2);
                    track->recordFifo->finishedRead(size1 + size2);
                }
            }
        }

        wait(20); // Poll every 20 ms
    }
}

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

            const int nGroups = owner.numGroupTracks.load(std::memory_order_relaxed);

            // Pre-clear group scratch buffers
            for (int g = 0; g < std::min(nGroups, 8); ++g)
                owner.groupScratches[g].clear();

            // ── Phase 1: render instruments only ───────────────────────────────
            // We need all instrument outputs available BEFORE running any effect
            // chain so that sidechain sources are fully rendered when their
            // consumers need them.  Instruments run exactly once per block.
            static thread_local std::vector<juce::AudioBuffer<float>> instrPool; // raw instrument out
            static thread_local std::vector<juce::AudioBuffer<float>> trackPool; // instrument + effects
            if ((int)instrPool.size() < nTracks)
                instrPool.resize(nTracks, juce::AudioBuffer<float>(nCh, bs));
            if ((int)trackPool.size() < nTracks)
                trackPool.resize(nTracks, juce::AudioBuffer<float>(nCh, bs));
            for (int t = 0; t < nTracks; ++t) {
                if (instrPool[t].getNumChannels() != nCh || instrPool[t].getNumSamples() != bs)
                    instrPool[t].setSize(nCh, bs, false, true, false);
                if (trackPool[t].getNumChannels() != nCh || trackPool[t].getNumSamples() != bs)
                    trackPool[t].setSize(nCh, bs, false, true, false);
            }

            for (int t = 0; t < nTracks; ++t)
            {
                instrPool[t].clear();
                if (auto* inst = owner.audioTracks[t]->activeInstrument.load(std::memory_order_acquire))
                    inst->processBlock(instrPool[t], owner.audioTracks[t]->midiBuffer);

                // GC instruments here (after processBlock, before anything else touches the track)
                while (auto opt = owner.audioTracks[t]->instrumentGarbageQueue.pop())
                {
                    InstrumentProcessor* dead = *opt;
                    juce::MessageManager::callAsync([dead] { delete dead; });
                }

                // ── 3.1  Audio clip warping ─────────────────────────────────────────
                // If the track has a warp-enabled arrangement clip with a loaded player,
                // MIX the time-stretched audio INTO instrPool[t] (additive with any
                // instrument output already rendered above).
                if (isArrMode
                    && currArrangement
                    && owner.audioTracks[t]->clipPlayerActive.load(std::memory_order_acquire)
                    && owner.audioTracks[t]->clipPlayer != nullptr
                    && owner.audioTracks[t]->clipPlayer->isLoaded())
                {
                    bool clipIsActive = false;
                    if (t < (int)currArrangement->tracks.size())
                    {
                        for (const auto& clip : currArrangement->tracks[t])
                        {
                            if (!clip.warpEnabled || clip.audioFilePath.isEmpty()) continue;

                            double clipStartBeat = (clip.startBar - 1.0) * 4.0;
                            double clipEndBeat   = clipStartBeat + clip.lengthBars * 4.0;

                            // Compute beat positions for this block (local to this scope).
                            int64_t offset_w       = owner.renderTransportOffset.load(std::memory_order_acquire);
                            int64_t transpSample_w = blockStart - offset_w;
                            if (transpSample_w < 0) transpSample_w = 0;
                            double spb_w       = owner.transportClock.getSamplesPerBeat();
                            double currentBeat = static_cast<double>(transpSample_w) / spb_w;
                            double endBeat     = currentBeat + static_cast<double>(bs) / spb_w;

                            if (clipEndBeat > currentBeat && clipStartBeat < endBeat)
                            {
                                clipIsActive = true;

                                // Update stretch ratio from projectBPM / clipBpm.
                                if (clip.clipBpm > 0.0)
                                {
                                    double projectBPM = owner.transportClock.getBpm();
                                    owner.audioTracks[t]->clipPlayer->setStretchRatio(
                                        projectBPM / clip.clipBpm);
                                }

                                // Pull stretched samples and MIX them into instrPool.
                                // (uses a temp buffer to avoid clearing the instrument's output)
                                static thread_local juce::AudioBuffer<float> warpScratch;
                                if (warpScratch.getNumChannels() < nCh || warpScratch.getNumSamples() < bs)
                                    warpScratch.setSize(nCh, bs, false, true, false);
                                warpScratch.clear();

                                owner.audioTracks[t]->clipPlayer->fillBlock(warpScratch, bs);

                                // Mix into instrPool (additive — allows blending with synth)
                                for (int ch = 0; ch < nCh; ++ch)
                                    juce::FloatVectorOperations::add(
                                        instrPool[t].getWritePointer(ch),
                                        warpScratch.getReadPointer(ch), bs);

                                break; // only the first active warp clip per track
                            }
                        }
                    }
                    if (!clipIsActive)
                    {
                        // Playhead moved outside the active clip — seek to keep the
                        // player cursor in sync for next entry (best-effort).
                        owner.audioTracks[t]->clipPlayer->seek(0.0, owner.transportClock.getSamplesPerBeat());
                    }
                }

                // ── 3.2  Comping ──────────────────────────────────────────────────────────
                // If this track has a CompPlayer active (takes-based comping), mix its
                // output into instrPool[t].  The comp path takes priority over the warp
                // path; if takes are non-empty, warpEnabled is ignored (see ideas_for_v0.2).
                if (isArrMode
                    && currArrangement
                    && owner.audioTracks[t]->compPlayerActive.load(std::memory_order_acquire)
                    && owner.audioTracks[t]->compPlayer != nullptr)
                {
                    // Drain GC queue (pointer-swapped old region lists)
                    owner.audioTracks[t]->compPlayer->drainRegionGarbage();

                    if (t < (int)currArrangement->tracks.size())
                    {
                        for (const auto& clip : currArrangement->tracks[t])
                        {
                            if (clip.takes.empty()) continue; // not a comp clip

                            double clipStartBeat = (clip.startBar - 1.0) * 4.0;
                            double clipEndBeat   = clipStartBeat + clip.lengthBars * 4.0;

                            // Compute beat positions for this block
                            int64_t off_c      = owner.renderTransportOffset.load(std::memory_order_acquire);
                            int64_t transpSmp  = blockStart - off_c;
                            if (transpSmp < 0) transpSmp = 0;
                            double  spb_c      = owner.transportClock.getSamplesPerBeat();
                            double  curBeat    = static_cast<double>(transpSmp) / spb_c;
                            double  endBeat    = curBeat + static_cast<double>(bs) / spb_c;

                            if (clipEndBeat > curBeat && clipStartBeat < endBeat)
                            {
                                double clipBeatOffset = curBeat - clipStartBeat;
                                if (clipBeatOffset < 0.0) clipBeatOffset = 0.0;

                                static thread_local juce::AudioBuffer<float> compScratch;
                                if (compScratch.getNumChannels() < nCh || compScratch.getNumSamples() < bs)
                                    compScratch.setSize(nCh, bs, false, true, false);
                                compScratch.clear();

                                owner.audioTracks[t]->compPlayer->fillBlock(
                                    compScratch, bs, clipBeatOffset, spb_c);

                                for (int ch = 0; ch < nCh; ++ch)
                                    juce::FloatVectorOperations::add(
                                        instrPool[t].getWritePointer(ch),
                                        compScratch.getReadPointer(ch), bs);

                                break; // one comp clip per track
                            }
                        }
                    }
                }
            }

            // ── Phase 2: effect chains (sidechain injected before effects run) ──
            // For each track:
            //   a) Copy instrPool[t] → trackPool[t]  (fresh program audio, pre-effects)
            //   b) If sidechain source is set, inject it into sidechain-capable effects
            //      — these effects now see instrPool[scSrc] AFTER ITS OWN EFFECTS have
            //        been applied (trackPool[scSrc] is complete at this point since we
            //        process in track order; for cross-order SC a second pass would be
            //        needed, but ordered loops cover the common kick→bass use-case).
            //   c) Run full effect chain (each effect runs exactly once).
            //   d) Apply PDC.
            for (int t = 0; t < nTracks; ++t)
            {
                // a) copy instrument output
                for (int ch = 0; ch < nCh; ++ch)
                    juce::FloatVectorOperations::copy(trackPool[t].getWritePointer(ch),
                                                      instrPool[t].getReadPointer(ch), bs);

                if (auto* vec = owner.audioTracks[t]->activeEffectChain.load(std::memory_order_acquire))
                {
                    // b) inject sidechain buffer into sidechain-capable effects before chain runs
                    const int scSrc = owner.audioTracks[t]->sidechainSourceTrack.load(std::memory_order_relaxed);
                    if (scSrc >= 0 && scSrc < nTracks && scSrc != t)
                    {
                        // Use trackPool[scSrc]: the source track's audio after its own effects.
                        // For t < scSrc this is instrPool (effects not yet applied);
                        // for t > scSrc this is the fully processed output — the most common case.
                        const juce::AudioBuffer<float>& srcBuf =
                            (scSrc < t) ? trackPool[scSrc] : instrPool[scSrc];

                        for (auto* effect : *vec)
                            if (effect && effect->wantsSidechain())
                                effect->setSidechainBuffer(&srcBuf);
                    }

                    // c) run full effect chain (each effect runs exactly once)
                    for (auto* effect : *vec)
                        if (effect) effect->processBlock(trackPool[t]);
                }

                // d) Plugin Delay Compensation
                if (owner.audioTracks[t]->pdcDelaySamples.load(std::memory_order_relaxed) > 0)
                    owner.audioTracks[t]->pdcLine.process(trackPool[t]);
            }

            // ── Phase 3: accumulate into master / groups / returns ──────────────
            for (int t = 0; t < nTracks; ++t)
            {
                const bool muted  = owner.audioTracks[t]->muted.load(std::memory_order_relaxed);
                const bool soloed = owner.audioTracks[t]->soloed.load(std::memory_order_relaxed);
                const bool audible = !muted && (!anySoloed || soloed);
                const float tGain = audible
                    ? owner.audioTracks[t]->gain.load(std::memory_order_relaxed)
                    : 0.0f;

                const juce::AudioBuffer<float>& trackScratch = trackPool[t];

                // ── Group bus or master accumulation (2.2) ────────────────────
                const int gBus = owner.audioTracks[t]->groupBusIndex.load(std::memory_order_relaxed);
                if (gBus >= 0 && gBus < nGroups)
                {
                    if (tGain > 0.0f || audible)
                        for (int ch = 0; ch < nCh; ++ch)
                        {
                            auto* dst = owner.groupScratches[gBus].getWritePointer(ch);
                            const auto* src = trackScratch.getReadPointer(ch);
                            for (int i = 0; i < bs; ++i) dst[i] += src[i] * tGain;
                        }
                }
                else
                {
                    if (tGain > 0.0f || audible)
                        for (int ch = 0; ch < nCh; ++ch)
                        {
                            auto* dst = owner.renderScratch.getWritePointer(ch);
                            const auto* src = trackScratch.getReadPointer(ch);
                            for (int i = 0; i < bs; ++i) dst[i] += src[i] * tGain;
                        }
                }

                // Sends are always post-fader to returns (regardless of group routing)
                if (tGain > 0.0f || audible)
                {
                    int nReturns = owner.numReturnTracks.load(std::memory_order_relaxed);
                    for (int r = 0; r < std::min(nReturns, 8); ++r) {
                        float tSendLevel = owner.audioTracks[t]->sendLevels[r].load(std::memory_order_relaxed);
                        if (tSendLevel > 0.0f) {
                            for (int ch = 0; ch < nCh; ++ch) {
                                auto* retDst = owner.returnScratches[r].getWritePointer(ch);
                                const auto* src = trackScratch.getReadPointer(ch);
                                for (int i = 0; i < bs; ++i)
                                    retDst[i] += src[i] * tGain * tSendLevel;
                            }
                        }
                    }
                }

                // Per-track RMS
                float sumRmsTrack = 0.0f;
                const auto* src0 = trackScratch.getReadPointer(0);
                for (int i = 0; i < bs; ++i) { float s = src0[i] * tGain; sumRmsTrack += s * s; }
                owner.audioTracks[t]->rmsLevel.store(std::sqrt(sumRmsTrack / bs), std::memory_order_relaxed);
            }

            // ── Process Group Track Effects & Accumulate to Master (2.2) ───
            for (int g = 0; g < std::min(nGroups, 8); ++g) {
                if (auto* vec = owner.groupTracks[g]->activeEffectChain.load(std::memory_order_acquire)) {
                    for (auto* effect : *vec) {
                        if (effect) effect->processBlock(owner.groupScratches[g]);
                    }
                }

                const float retGain = owner.groupTracks[g]->gain.load(std::memory_order_relaxed);
                float sumRmsGroup = 0.0f;
                for (int ch = 0; ch < nCh; ++ch) {
                    auto* dst = owner.renderScratch.getWritePointer(ch);
                    const auto* src = owner.groupScratches[g].getReadPointer(ch);
                    for (int i = 0; i < bs; ++i) {
                        float s = src[i] * retGain;
                        dst[i] += s;
                        if (ch == 0) sumRmsGroup += s * s;
                    }
                }
                owner.groupTracks[g]->rmsLevel.store(std::sqrt(sumRmsGroup / bs), std::memory_order_relaxed);
            }

            // ── Process Return Track Effects & Accumulate to Master ───────
            int nReturns = owner.numReturnTracks.load(std::memory_order_relaxed);
            for (int r = 0; r < std::min(nReturns, 8); ++r) {
                if (auto* vec = owner.returnTracks[r]->activeEffectChain.load(std::memory_order_acquire)) {
                    for (auto* effect : *vec) {
                        if (effect) effect->processBlock(owner.returnScratches[r]);
                    }
                }

                const float retGain = owner.returnTracks[r]->gain.load(std::memory_order_relaxed);
                float sumRmsReturn = 0.0f;
                for (int ch = 0; ch < nCh; ++ch) {
                    auto* dst = owner.renderScratch.getWritePointer(ch);
                    const auto* src = owner.returnScratches[r].getReadPointer(ch);
                    for (int i = 0; i < bs; ++i) {
                        float s = src[i] * retGain;
                        dst[i] += s;
                        if (ch == 0) sumRmsReturn += s * s;
                    }
                }
                owner.returnTracks[r]->rmsLevel.store(std::sqrt(sumRmsReturn / bs), std::memory_order_relaxed);
                owner.returnScratches[r].clear(); // clear for next block
            }


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
                        // Signal RecordingThread that a loop-wrap occurred so it can
                        // finalise the current take and open a new one (3.2 loop record).
                        owner.loopWrapCounter.fetch_add(1, std::memory_order_release);
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
    : renderThread(*this), recordingThread(*this)
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
    returnTracks.push_back(std::make_unique<Track>());
    numReturnTracks.store(1, std::memory_order_release);

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

    // Any knob/slider/button release inside the device chain marks the project dirty.
    deviceView.onParamChanged = [this] { markDirty(); };

    deviceView.canAddAutomation = [this]() -> bool {
        if (selectedTrackIndex < 0 || selectedTrackIndex >= (int)audioTracks.size()) return false;
        // Arrangement mode: need a selected clip
        if (currentView == DAWView::Arrangement)
            return selectedArrangementClip != nullptr;
        // Session mode: need an in-bounds clip with hasClip == true
        if (selectedSceneIndex < 0) return false;
        if (selectedTrackIndex >= (int)clipGrid.size()) return false;
        if (selectedSceneIndex >= (int)clipGrid[selectedTrackIndex].size()) return false;
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
            || selectedSceneIndex >= (int)clipGrid[selectedTrackIndex].size()) return false;
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
            if (selectedSceneIndex >= 0
                && selectedTrackIndex < (int)clipGrid.size()
                && selectedSceneIndex < (int)clipGrid[selectedTrackIndex].size())
            {
                auto& clip = clipGrid[selectedTrackIndex][selectedSceneIndex];
                if (!clip.hasClip) return; // nothing to automate
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
                && selectedSceneIndex < (int)clipGrid[selectedTrackIndex].size()
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
        // ── 5.1 Control Surface ────────────────────────────────────────────────
        controlSurfaceManager.notifyTransport(true, transportClock.getIsRecording());
    };

    topBar->stopBtn.onClick = [this] {
        transportClock.stop();
        renderTransportOffset.store(appBuffer.renderBlockPosition.load(std::memory_order_acquire), std::memory_order_release);
        int nTracks = numActiveTracks.load(std::memory_order_relaxed);
        for (int t = 0; t < nTracks; ++t)
            audioTracks[t]->commandQueue.push({ TrackCommand::Type::StopPattern, nullptr, 0 });
        for (int t = 0; t < nTracks; ++t)
            for (int s = 0; s < (int)clipGrid[t].size(); ++s)
                if (clipGrid[t][s].isPlaying) {
                    clipGrid[t][s].isPlaying = false;
                    sessionView.setClipData(t, s, clipGrid[t][s]);
                }
        sessionView.setSceneActive(-1, false);
        // ── 5.1 Control Surface ────────────────────────────────────────────────
        controlSurfaceManager.notifyTransport(false, false);
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
    sessionView.onAddScene = [this](int t) {
        if (t < 0 || t >= (int)audioTracks.size()) return;

        // Create a pre-filled ClipData so the slot shows immediately as a clip.
        int s = (int)clipGrid[t].size(); // index of the new slot
        ClipData newClip;
        newClip.hasClip         = true;
        newClip.name            = "Pattern " + juce::String(s + 1);
        newClip.colour          = juce::Colour::fromHSV(
            std::fmod((float)(t * 47 + s * 31) / 360.0f, 1.0f), 0.65f, 0.70f, 1.0f);
        newClip.euclideanSteps  = 16;
        newClip.euclideanPulses = 4;
        newClip.patternMode     = (trackInstruments[t] == "Oscillator") ? "pianoroll"
                                : (trackInstruments[t] == "DrumRack")   ? "drumrack"
                                                                         : "euclidean";
        clipGrid[t].push_back(newClip);  // grow data model with filled clip
        sessionView.addSceneToTrack(t);  // grow UI slot
        sessionView.setClipData(t, s, clipGrid[t][s]); // paint the slot as filled
        // ── 5.2 Control Surface: new clip slot → HasClip LED ─────────────────
        controlSurfaceManager.notifyClipState(t, s, ClipState::HasClip);
        markDirty();
    };

    sessionView.onCreateClip = [this](int t, int s) {
        if (t < 0 || t >= (int)audioTracks.size()) return;
        // Ensure the scene row exists in the data model
        while ((int)clipGrid[t].size() <= s) {
            clipGrid[t].emplace_back();
            sessionView.addSceneToTrack(t);
        }
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
        // ── 5.2 Control Surface: new clip slot → HasClip LED ─────────────────
        controlSurfaceManager.notifyClipState(t, s, ClipState::HasClip);
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
        for (int i = 0; i < (int)clipGrid[t].size(); ++i) {
            if (i != s && clipGrid[t][i].isPlaying) {
                clipGrid[t][i].isPlaying = false;
                sessionView.setClipData(t, i, clipGrid[t][i]);
                // ── 5.1 Control Surface: notify observers of clip stop ────────────────
                controlSurfaceManager.notifyClipState(t, i, clipGrid[t][i].hasClip ? ClipState::HasClip : ClipState::Empty);
            }
        }

        audioTracks[t]->commandQueue.push(
            { TrackCommand::Type::PlayPattern, p, scheduleTime });
        clip.isPlaying = true;
        sessionView.setClipData(t, s, clip);
        // ── 5.1 Control Surface: notify observers of clip launch ───────────────
        controlSurfaceManager.notifyClipState(t, s, ClipState::Playing);
    };

    sessionView.onPauseClip = [this](int t, int s) {
        if (t < 0 || t >= (int)audioTracks.size()) return;
        auto& clip = clipGrid[t][s];
        if (!clip.isPlaying) return;

        audioTracks[t]->commandQueue.push({ TrackCommand::Type::StopPattern, nullptr, 0 });
        clip.isPlaying = false;
        sessionView.setClipData(t, s, clip);
        // ── 5.1 Control Surface: notify observers of clip stop ────────────────
        controlSurfaceManager.notifyClipState(t, s,
            clip.hasClip ? ClipState::HasClip : ClipState::Empty);

        // If no other clip is playing on any track, stop the transport clock
        bool anyPlaying = false;
        int nTracks = numActiveTracks.load(std::memory_order_relaxed);
        for (int ti = 0; ti < nTracks && !anyPlaying; ++ti)
            for (int si = 0; si < (int)clipGrid[ti].size() && !anyPlaying; ++si)
                if (clipGrid[ti][si].isPlaying) anyPlaying = true;

        if (!anyPlaying)
            transportClock.stop();
    };

    sessionView.onSelectTrack = [this](int t) {
        // Group tracks use indices 2000+, return tracks 1000+
        // showDeviceEditorForTrack handles all ranges safely
        if (t >= 2000)
        {
            selectedTrackIndex = t;
            selectedSceneIndex = -1;
            showDeviceEditorForTrack(t);
            return;
        }
        selectedTrackIndex = t;
        selectedSceneIndex = -1;
        sessionView.setClipSelected(t, -1);
        sessionView.setTrackSelected(t);
        resized();
        showDeviceEditorForTrack(t);
    };

    sessionView.onDeleteClip = [this](int t, int s) {
        if (t < 0 || t >= (int)audioTracks.size()) return;
        if (s < 0 || s >= (int)clipGrid[t].size()) return;

        auto& clip = clipGrid[t][s];
        markDirty();

        if (clip.isPlaying)
            audioTracks[t]->commandQueue.push({ TrackCommand::Type::StopPattern, nullptr, 0 });

        // ── 5.2 Control Surface: notify slot becomes empty BEFORE erasing ─────
        controlSurfaceManager.notifyClipState(t, s, ClipState::Empty);

        // Erase from data model (shifts subsequent elements down)
        clipGrid[t].erase(clipGrid[t].begin() + s);

        // Remove the slot from the UI column and re-wire subsequent indices
        sessionView.removeSceneFromTrack(t, s);

        // Clear pattern editor if this was the selected scene
        if (selectedTrackIndex == t && selectedSceneIndex == s) {
            patternEditor.setVisible(false);
            selectedSceneIndex = -1;
            resized();
        } else if (selectedTrackIndex == t && selectedSceneIndex > s) {
            // Adjust selection index since all scenes above shifted down
            selectedSceneIndex--;
        }
    };

    sessionView.onDuplicateClip = [this](int t, int s) {
        if (t < 0 || t >= (int)audioTracks.size()) return;
        auto& sourceClip = clipGrid[t][s];
        if (!sourceClip.hasClip) return;
        
        int nextEmpty = -1;
        for (int ns = s + 1; ns < (int)clipGrid[t].size(); ++ns) {
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
            // ── 5.2 Control Surface: duplicated slot → HasClip LED ────────────
            controlSurfaceManager.notifyClipState(t, nextEmpty, ClipState::HasClip);
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

        // ── Step 1b: Pre-null the effect chain atomically so the render thread
        // cannot enter any effect's processBlock after this point.  Without this,
        // a slow VST3 effect could keep the render thread busy past the 2 s
        // stopThread timeout, causing a force-kill and corrupted state.
        if (auto* oldVec = audioTracks[trackIndex]->activeEffectChain.exchange(nullptr, std::memory_order_acq_rel)) {
            audioTracks[trackIndex]->effectVectorGarbageQueue.push(oldVec);
        }

        // ── Step 2: Stop the render thread with a generous timeout.
        // Both activeInstrument and effectChain slots are already nullptr, so the
        // render thread skips this track's DSP entirely — 2 s is more than enough.
        bool wasRunning = renderThread.isThreadRunning();
        if (wasRunning) renderThread.stopThread(2000);

        // ── Step 3: Async-delete the old instrument on the message thread.
        // This matches the GC path used by the render thread and is safe for
        // PluginInstrumentAdapters whose VST3 destructors need the message loop.
        if (deadInst)
            juce::MessageManager::callAsync([deadInst] { delete deadInst; });

        // ── Step 4: Drain remaining garbage (patterns, effects) synchronously
        // now that the render thread is stopped.  Effect slots are already nullptr
        // (pre-nulled above) so clear() will just drain effectGarbageQueue.
        audioTracks[trackIndex]->clear();

        // ── Step 5: Null any dangling selectedArrangementClip pointer that points
        // into arrangementTracks[trackIndex] BEFORE we erase that vector entry.
        // Failing to do this causes a use-after-free crash when subsequent code
        // (automation overlay, showDeviceEditorForTrack, etc.) dereferences the ptr.
        if (selectedArrangementClip != nullptr) {
            for (const auto& clip : arrangementTracks[trackIndex]) {
                if (&clip == selectedArrangementClip) {
                    selectedArrangementClip = nullptr;
                    arrangementView.setSelectedClip(nullptr);
                    break;
                }
            }
        }

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

        recalculatePDC();
        resized();
        if (wasRunning) renderThread.startThread();
        // ── 5.1 Control Surface: grid layout changed ──────────────────────────
        controlSurfaceManager.notifyLayout();
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
            // Stop any currently-playing clip in this track (except the target row)
            for (int s = 0; s < (int)clipGrid[t].size(); ++s) {
                if (s != sceneIdx && clipGrid[t][s].isPlaying) {
                    clipGrid[t][s].isPlaying = false;
                    sessionView.setClipData(t, s, clipGrid[t][s]);
                    // ── 5.1 Control Surface: notify observers of clip stop ────────────────
                    controlSurfaceManager.notifyClipState(t, s, clipGrid[t][s].hasClip ? ClipState::HasClip : ClipState::Empty);
                }
            }

            // Only launch if this track actually has a scene at sceneIdx
            if (sceneIdx < (int)clipGrid[t].size() && clipGrid[t][sceneIdx].hasClip) {
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
                // ── 5.1 Control Surface ────────────────────────────────────────
                controlSurfaceManager.notifyClipState(t, sceneIdx, ClipState::Playing);
            } else {
                // Track has no clip at this row — stop it silently
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
                newInst->setHostTrack(static_cast<void*>(audioTracks[targetIdx].get()));
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
            clipGrid.push_back(std::vector<ClipData>());
            trackInstruments.push_back(type);
            loadedFiles.push_back(juce::File{});
            arrangementTracks.push_back({});
            targetIdx = numActiveTracks.fetch_add(1, std::memory_order_release);
            sessionView.addTrack(TrackType::Audio, "Track " + juce::String(targetIdx + 1));
            sessionView.gridContent.columns[targetIdx]->header.hasInstrument = true;
            sessionView.gridContent.columns[targetIdx]->header.instrumentName = type;
            sessionView.gridContent.columns[targetIdx]->header.repaint();
            syncArrangementFromSession();
            
            if (auto* current = audioTracks[targetIdx]->activeInstrument.load(std::memory_order_acquire)) {
                current->closeUI();
            }
            
            // Instantiate modular instrument
            auto newInst = InstrumentFactory::create(type);
            if (newInst) {
                newInst->setHostTrack(static_cast<void*>(audioTracks[targetIdx].get()));
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
        // ── 5.1 Control Surface: track added ──────────────────────────────────
        controlSurfaceManager.notifyLayout();
    };

    sessionView.onEffectDropped = [this](int trackIdx, const juce::String& type) {
        int nTracks = numActiveTracks.load(std::memory_order_relaxed);
        Track* targetTrack = nullptr;

        if (trackIdx >= 2000) {
            int grpIdx = trackIdx - 2000;
            if (grpIdx >= 0 && grpIdx < numGroupTracks.load())
                targetTrack = groupTracks[grpIdx].get();
        } else if (trackIdx >= 1000) {
            int retIdx = trackIdx - 1000;
            if (retIdx >= 0 && retIdx < numReturnTracks.load())
                targetTrack = returnTracks[retIdx].get();
        } else if (trackIdx >= 0 && trackIdx < nTracks) {
            targetTrack = audioTracks[trackIdx].get();
        } else {
            return;
        }

        auto newEffect = EffectFactory::create(type);
        if (newEffect && targetTrack) {
            newEffect->prepareToPlay(currentSampleRate);
            newEffect->registerAutomationParameters(targetTrack);

            std::vector<EffectProcessor*>* newChain = new std::vector<EffectProcessor*>();
            if (auto* oldVec = targetTrack->activeEffectChain.load(std::memory_order_acquire)) {
                *newChain = *oldVec;
            }
            newChain->push_back(newEffect.release());
            
            if (auto* oldVec = targetTrack->activeEffectChain.exchange(newChain, std::memory_order_acq_rel)) {
                targetTrack->effectVectorGarbageQueue.push(oldVec);
            }
        }

        // Always select the track and show its device panel immediately
        selectedTrackIndex = trackIdx;
        if (trackIdx < 1000) selectedSceneIndex = -1;
        sessionView.setTrackSelected(trackIdx);
        if (trackIdx != 999) sessionView.setClipSelected(trackIdx, -1);
        
        showDeviceEditorForTrack(trackIdx);
        recalculatePDC();
        resized();
        markDirty();
    };

    // ── Effect-drop fallback for Arrangement mode ─────────────────────────────
    // SessionView is still the DragAndDropTarget even when the Arrangement view is
    // visible.  Its column hit-test fails in that context because the columns are
    // not in view.  This callback returns the currently selected track so the drop
    // can still land on the right track without the user having to be in Session view.
    sessionView.getSelectedTrackIndex = [this]() -> int { return selectedTrackIndex; };

    // ── Mixer Volume Callbacks ────────────────────────────────────────
    // These are the ONLY paths that write to the audio-thread gain atomics.
    // Without them the faders are purely cosmetic.
    sessionView.onTrackVolumeChanged = [this](int t, float gain) {
        if (t >= 0 && t < (int)audioTracks.size()) {
            audioTracks[t]->gain.store(gain, std::memory_order_relaxed);
            markDirty();
        }
    };

    sessionView.onTrackSendChanged = [this](int t, int retIdx, float level) {
        if (t >= 0 && t < (int)audioTracks.size()
            && retIdx >= 0 && retIdx < 8) {
            audioTracks[t]->sendLevels[retIdx].store(level, std::memory_order_relaxed);
            markDirty();
        }
    };

    sessionView.onReturnRackDropped = [this]() {
        int newRetIdx = numReturnTracks.load(std::memory_order_relaxed);
        if (newRetIdx >= 8) return; // max 8 return tracks (matches sendLevels array)

        // Create a new return track
        returnTracks.push_back(std::make_unique<Track>());
        numReturnTracks.store(newRetIdx + 1, std::memory_order_release);

        // Allocate its scratch buffer
        returnScratches[newRetIdx].setSize(2, currentBufferSize > 0 ? currentBufferSize : 512);
        returnScratches[newRetIdx].clear();

        // Name it Return A, B, C…
        juce::String retName = "Return ";
        retName += (char)('A' + newRetIdx);

        // Add the return column to the session view
        sessionView.addReturnTrack(retName);

        // Add a send knob to ALL existing track columns for this new return
        sessionView.gridContent.addSendKnobToAllColumns(newRetIdx);
        sessionView.updateContentSize();
        sessionView.resized();
        markDirty();
    };

    sessionView.onMasterVolumeChanged = [this](float gain) {
        masterTrack.gain.store(gain, std::memory_order_relaxed);
        markDirty();
    };

    sessionView.onReturnVolumeChanged = [this](int retIdx, float gain) {
        if (retIdx >= 0 && retIdx < numReturnTracks.load()) {
            returnTracks[retIdx]->gain.store(gain, std::memory_order_relaxed);
            markDirty();
        }
    };

    sessionView.onDeleteReturnTrack = [this](int retIdx) {
        int currentCount = numReturnTracks.load(std::memory_order_relaxed);
        if (retIdx < 0 || retIdx >= currentCount) return;

        // ── Step 1: Atomically null the effect chain so the render thread
        // skips processBlock on this return bus immediately.
        Track* deadTrack = returnTracks[retIdx].get();
        if (auto* oldVec = deadTrack->activeEffectChain.exchange(nullptr, std::memory_order_acq_rel))
            deadTrack->effectVectorGarbageQueue.push(oldVec);

        // ── Step 2: Atomically decrement the count.  The render thread reads
        // numReturnTracks before indexing returnTracks[r], so after this store
        // it will never touch slot retIdx again in the next block iteration.
        numReturnTracks.fetch_sub(1, std::memory_order_release);

        // ── Step 3: Compact the scratch buffers now (message thread only;
        // render thread no longer reads beyond [currentCount-2]).
        for (int i = retIdx; i < currentCount - 1; ++i)
            returnScratches[i] = std::move(returnScratches[i + 1]);
        returnScratches[currentCount - 1].setSize(0, 0);

        // ── Step 4: Extract ownership and erase from the vector, then
        // async-delete the Track object on the message thread (safe for any
        // VST3 plugin destructors that need the message loop).
        std::unique_ptr<Track> owned = std::move(returnTracks[retIdx]);
        returnTracks.erase(returnTracks.begin() + retIdx);
        Track* raw = owned.release();
        juce::MessageManager::callAsync([raw] { delete raw; });

        markDirty();
    };

    // ── Group Track Callbacks (2.2) ───────────────────────────────────────────
    sessionView.onAddGroupTrack = [this] {
        addGroupTrack();
    };

    sessionView.onDeleteGroupTrack = [this](int groupIdx) {
        deleteGroupTrack(groupIdx);
    };

    sessionView.onRouteTrackToGroup = [this](int trackIdx, int groupIdx) {
        // groupIdx == -1 means "route to master"
        const int nTracks = numActiveTracks.load(std::memory_order_relaxed);
        const int nGroups = numGroupTracks.load(std::memory_order_relaxed);
        if (trackIdx < 0 || trackIdx >= nTracks) return;
        if (groupIdx != -1 && (groupIdx < 0 || groupIdx >= nGroups)) return;
        audioTracks[trackIdx]->groupBusIndex.store(groupIdx, std::memory_order_release);
        markDirty();
    };

    sessionView.onGroupVolumeChanged = [this](int groupIdx, float gain) {
        if (groupIdx >= 0 && groupIdx < numGroupTracks.load()) {
            groupTracks[groupIdx]->gain.store(gain, std::memory_order_relaxed);
            markDirty();
        }
    };

    // ── Assign track to group from right-click header submenu ─────────────────
    sessionView.onAssignToGroup = [this](int trackIdx, int groupIdx) {
        const int nTracks = numActiveTracks.load(std::memory_order_relaxed);
        if (trackIdx < 0 || trackIdx >= nTracks) return;

        // groupIdx == numGroupTracks means "create a new group for this track"
        int resolvedGroup = groupIdx;
        if (groupIdx >= numGroupTracks.load(std::memory_order_relaxed))
        {
            addGroupTrack(); // increments numGroupTracks, creates engine Track + UI column
            resolvedGroup = numGroupTracks.load(std::memory_order_relaxed) - 1;
        }

        audioTracks[trackIdx]->groupBusIndex.store(resolvedGroup, std::memory_order_release);
        sessionView.setTrackParentGroup(trackIdx, resolvedGroup);
        refreshGroupBadges();
        markDirty();
    };

    // ── Ungroup all tracks from a group ───────────────────────────────────────
    sessionView.onUngroupAll = [this](int groupIdx) {
        const int nTracks = numActiveTracks.load(std::memory_order_relaxed);
        for (int t = 0; t < nTracks; ++t)
            if (audioTracks[t]->groupBusIndex.load(std::memory_order_relaxed) == groupIdx)
            {
                audioTracks[t]->groupBusIndex.store(-1, std::memory_order_release);
                sessionView.setTrackParentGroup(t, -1);
            }
        refreshGroupBadges();
        markDirty();
    };

    // ── Rename group track ────────────────────────────────────────────────────
    sessionView.onRenameGroupTrack = [this](int groupIdx, const juce::String& name) {
        if (groupIdx >= 0 && groupIdx < (int)groupTrackNames.size())
            groupTrackNames[groupIdx] = name;
        markDirty();
    };

    // ── Sidechain Source Callback (2.1) ───────────────────────────────────────
    sessionView.onSidechainSourceChanged = [this](int targetTrackIdx, int sourceTrackIdx) {
        setSidechainSource(targetTrackIdx, sourceTrackIdx);
    };

    sessionView.onTrackMuteChanged = [this](int t, bool muted) {
        if (t >= 0 && t < (int)audioTracks.size()) {
            audioTracks[t]->muted.store(muted, std::memory_order_relaxed);
            markDirty();
        }
    };

    sessionView.onTrackSoloChanged = [this](int t, bool soloed) {
        if (t >= 0 && t < (int)audioTracks.size()) {
            audioTracks[t]->soloed.store(soloed, std::memory_order_relaxed);
            markDirty();
        }
    };

    sessionView.onTrackArmChanged = [this](int t, bool armed) {
        if (t >= 0 && t < (int)audioTracks.size()) {
            audioTracks[t]->isArmedForRecord.store(armed, std::memory_order_relaxed);
            markDirty();
        }
    };

    // ── Rename / Color callbacks ──────────────────────────────────────────────
    sessionView.onRenameClip = [this](int t, int s, const juce::String& name) {
        if (t >= 0 && t < (int)clipGrid.size() && s >= 0 && s < (int)clipGrid[t].size()) {
            clipGrid[t][s].name = name;
            sessionView.setClipData(t, s, clipGrid[t][s]);
            markDirty();
        }
    };

    sessionView.onSetClipColour = [this](int t, int s, juce::Colour c) {
        if (t >= 0 && t < (int)clipGrid.size() && s >= 0 && s < (int)clipGrid[t].size()) {
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

    arrangementView.onInstrumentDropped = [this](int trackIdx, const juce::String& type) {
        if (sessionView.onInstrumentDropped) sessionView.onInstrumentDropped(trackIdx, type);
    };

    arrangementView.onEffectDropped = [this](int trackIdx, const juce::String& type) {
        if (sessionView.onEffectDropped) sessionView.onEffectDropped(trackIdx, type);
    };

    arrangementView.onTrackSelected = [this](int trackIdx) {
        if (trackIdx < 0 || trackIdx >= (int)audioTracks.size()) return;
        selectedTrackIndex = trackIdx;
        arrangementView.setSelectedTrack(trackIdx);
        showDeviceEditorForTrack(trackIdx);
        resized(); // re-layout bottom panel if device view changed height
    };

    arrangementView.onTrackArmChanged = [this](int trackIdx, bool armed) {
        if (trackIdx >= 0 && trackIdx < (int)audioTracks.size()) {
            audioTracks[trackIdx]->isArmedForRecord.store(armed, std::memory_order_relaxed);
            // Also sync the SessionView state
            sessionView.gridContent.columns[trackIdx]->setArmed(armed);
            markDirty();
        }
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

    // ── Comping (3.2): swipe-to-comp gesture ─────────────────────────────────
    arrangementView.onCompRegionSwiped = [this](int trackIdx, int clipIdx, int takeIdx,
                                                 double startBeat, double endBeat)
    {
        onCompRegionSwiped(trackIdx, clipIdx, takeIdx, startBeat, endBeat);
        // Refresh the take lane overlay so the new region is drawn immediately
        if (trackIdx < (int)arrangementTracks.size() && !arrangementTracks[trackIdx].empty())
            arrangementView.setTakeLane(trackIdx, clipIdx, &arrangementTracks[trackIdx]);
    };

    // ── Comping (3.2): "Show Take Lanes" right-click menu item ────────────────
    arrangementView.onShowTakeLanes = [this](int trackIdx, ArrangementClip* clip)
    {
        if (trackIdx < 0 || trackIdx >= (int)arrangementTracks.size()) return;
        auto& trackClips = arrangementTracks[trackIdx];
        // Find the clip index
        int clipIdx = -1;
        for (int ci = 0; ci < (int)trackClips.size(); ++ci)
            if (&trackClips[ci] == clip) { clipIdx = ci; break; }
        if (clipIdx >= 0)
            arrangementView.setTakeLane(trackIdx, clipIdx, &trackClips);
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

    // 2. Stop the render and recording threads before shutting down the audio device.
    recordingThread.stopThread(500);
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
    
    for (auto& buf : returnScratches) {
        buf.setSize(2, samplesPerBlockExpected);
        buf.clear();
    }

    // Pre-fill the ring with silence so the first callback never underruns.
    juce::AudioBuffer<float> silence(2, samplesPerBlockExpected);
    silence.clear();
    for (int i = 0; i < AppAudioBuffer::RING_CAPACITY_BLOCKS - 1; ++i)
        appBuffer.writeBlock(silence);

    // Start the threads (they will wait on events or loop)
    if (!renderThread.isThreadRunning())
        renderThread.startThread();
    if (!recordingThread.isThreadRunning())
        recordingThread.startThread();
        
    for (int t = 0; t < (int)audioTracks.size(); ++t) {
        if (audioTracks[t]->recordFifo == nullptr) {
            audioTracks[t]->recordFifo = std::make_unique<juce::AbstractFifo>(currentSampleRate * 2.0);
            audioTracks[t]->recordBuffer.resize(currentSampleRate * 2.0, 0.0f);
        }
        if (audioTracks[t]->monitorFifo == nullptr) {
            // Monitor buffer can be much smaller (e.g. 50ms) to reduce latency and memory
            int monitorSize = (int)(currentSampleRate * 0.1);
            audioTracks[t]->monitorFifo = std::make_unique<juce::AbstractFifo>(monitorSize);
            audioTracks[t]->monitorBuffer.resize(monitorSize, 0.0f);
        }
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

    // ── Live Audio Recording Capture ──
    const bool isRecording = transportClock.getIsRecording();
    const int nTracks = numActiveTracks.load(std::memory_order_relaxed);
    
    for (int t = 0; t < nTracks; ++t) {
        bool armed = audioTracks[t]->isArmedForRecord.load(std::memory_order_relaxed);
        bool monitoring = audioTracks[t]->monitorEnabled.load(std::memory_order_relaxed);
        
        if ((armed && isRecording) || monitoring) {
            int inputCh = audioTracks[t]->recordInputChannel.load(std::memory_order_relaxed);
            // Fallback to channel 0 if out of bounds (or handle stereo later)
            if (inputCh >= bufferToFill.buffer->getNumChannels()) inputCh = 0;
            const float* inputData = bufferToFill.buffer->getReadPointer(inputCh, bufferToFill.startSample);
            
            if (armed && isRecording && audioTracks[t]->recordFifo != nullptr) {
                int start1, size1, start2, size2;
                audioTracks[t]->recordFifo->prepareToWrite(numSamples, start1, size1, start2, size2);
                
                if (size1 > 0) std::copy(inputData, inputData + size1, audioTracks[t]->recordBuffer.begin() + start1);
                if (size2 > 0) std::copy(inputData + size1, inputData + size1 + size2, audioTracks[t]->recordBuffer.begin() + start2);
                
                audioTracks[t]->recordFifo->finishedWrite(size1 + size2);
            }
            
            if (monitoring && audioTracks[t]->monitorFifo != nullptr) {
                int start1, size1, start2, size2;
                audioTracks[t]->monitorFifo->prepareToWrite(numSamples, start1, size1, start2, size2);
                
                if (size1 > 0) std::copy(inputData, inputData + size1, audioTracks[t]->monitorBuffer.begin() + start1);
                if (size2 > 0) std::copy(inputData + size1, inputData + size1 + size2, audioTracks[t]->monitorBuffer.begin() + start2);
                
                audioTracks[t]->monitorFifo->finishedWrite(size1 + size2);
            }
        }
    }

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
    recordingThread.stopThread(200);
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
        
        state.isArmed = audioTracks[t]->isArmedForRecord.load(std::memory_order_relaxed);
        
        // Gather clips present on this track
        for (int s = 0; s < (int)clipGrid[t].size(); ++s) {
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
    for (int r = 0; r < numReturnTracks.load(); ++r) gcTrack(*returnTracks[r]);
    for (int t = 0; t < nTracks; ++t) gcTrack(*audioTracks[t]);

    // ── GC: Instrument internal buffers & Effect garbage ─────────────────────
    auto gcProcessors = [](Track& t) {
        if (auto* inst = t.activeInstrument.load(std::memory_order_relaxed)) {
            inst->processGarbage();
        }
        if (auto* vec = t.activeEffectChain.load(std::memory_order_relaxed)) {
            for (auto* effect : *vec) {
                if (effect) effect->processGarbage();
            }
        }
        // Drain replaced instruments (e.g. PluginInstrumentAdapter with open windows).
        // NOTE: instrumentGarbageQueue is drained on the RENDER thread (see RenderThread::run)
        // to avoid use-after-free. Do NOT drain it here.
        while (auto opt = t.effectGarbageQueue.pop()) {
            delete *opt;
        }
        while (auto opt = t.effectVectorGarbageQueue.pop()) {
            delete *opt;
        }
    };
    
    for (int t = 0; t < nTracks; ++t) {
        gcProcessors(*audioTracks[t]);
    }
    for (int r = 0; r < numReturnTracks.load(); ++r) {
        gcProcessors(*returnTracks[r]);
    }
    gcProcessors(masterTrack);

    // ── GC: Old plugin instances ──────────────────────────────────────────────
    while (auto opt = pluginGarbageQueue.pop())
        delete *opt;

    // ── Delay tempo sync: push current BPM into every DelayEffect ────────────
    // Called at 30 Hz — cheap enough (just an atomic store per effect).
    {
        const double currentBpm = transportClock.getBpm();
        auto propagateBpm = [&](Track& t) {
            if (auto* vec = t.activeEffectChain.load(std::memory_order_acquire))
                for (auto* eff : *vec)
                    if (auto* del = dynamic_cast<DelayEffect*>(eff))
                        del->setBpm(currentBpm);
        };
        propagateBpm(masterTrack);
        for (int r = 0; r < numReturnTracks.load(); ++r) propagateBpm(*returnTracks[r]);
        for (int t = 0; t < nTracks; ++t)               propagateBpm(*audioTracks[t]);
    }


    // ── 3.2 CompPlayer activation poll ────────────────────────────────────────
    // Once all take AudioClipPlayers have finished their background loads,
    // flip compPlayerActive so the render thread starts calling fillBlock().
    for (int t = 0; t < nTracks; ++t)
    {
        auto* track = audioTracks[t].get();
        if (!track) continue;
        if (track->compPlayerActive.load(std::memory_order_relaxed)) continue; // already active
        if (!track->compPlayer) continue;
        if (track->compPlayer->getNumTakes() > 0 && track->compPlayer->allPlayersLoaded())
        {
            track->compPlayerActive.store(true, std::memory_order_release);
            DBG("timerCallback: compPlayerActive = true for track " + juce::String(t));
        }
    }

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

    // ── Push RMS into Return tracks and Master faders ─────────────────────────
    {
        for (int r = 0; r < numReturnTracks.load(); ++r) {
            float retRaw = returnTracks[r]->rmsLevel.load(std::memory_order_relaxed);
            // We need a member array of level displays if we have multiple return tracks
            // For now, let's just push it to SessionView and let it smooth it if it wants,
            // or just use retRaw directly for multiple tracks since we don't have returnLevelDisplay[8] yet.
            sessionView.setReturnRmsLevel(r, retRaw);
        }
        float masterRaw = masterTrack.rmsLevel.load  (std::memory_order_relaxed);
        applyDecay (masterLevelDisplay, masterRaw);
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

// ─────────────────────────────────────────────────────────────────────────────
//  Control Surface read-only accessors (5.2)
// ─────────────────────────────────────────────────────────────────────────────

ClipState MainComponent::getClipState (int trackIdx, int sceneIdx) const
{
    if (trackIdx < 0 || trackIdx >= (int)clipGrid.size()) return ClipState::Empty;
    const auto& track = clipGrid[trackIdx];
    if (sceneIdx < 0 || sceneIdx >= (int)track.size())    return ClipState::Empty;
    const auto& clip = track[sceneIdx];
    if (!clip.hasClip)    return ClipState::Empty;
    if (clip.isPlaying)   return ClipState::Playing;
    return ClipState::HasClip;
}

int MainComponent::getNumScenes (int trackIdx) const
{
    if (trackIdx < 0 || trackIdx >= (int)clipGrid.size()) return 0;
    return static_cast<int> (clipGrid[trackIdx].size());
}

// ─────────────────────────────────────────────────────────────────────────────
//  Control Surface action proxies (5.2)
//  Forward hardware controller actions to the existing SessionView callbacks.
// ─────────────────────────────────────────────────────────────────────────────

void MainComponent::csLaunchClip (int trackIdx, int sceneIdx)
{
    juce::MessageManager::callAsync ([this, trackIdx, sceneIdx] {
        if (trackIdx < 0 || trackIdx >= (int)audioTracks.size()) return;
        
        bool isEmpty = true;
        bool isPlaying = false;

        if (sceneIdx >= 0 && sceneIdx < (int)clipGrid[trackIdx].size())
        {
            isEmpty = !clipGrid[trackIdx][sceneIdx].hasClip;
            isPlaying = clipGrid[trackIdx][sceneIdx].isPlaying;
        }

        if (isEmpty)
        {
            if (sessionView.onCreateClip) sessionView.onCreateClip (trackIdx, sceneIdx);
            return;
        }

        if (isPlaying)
        {
            if (sessionView.onPauseClip) sessionView.onPauseClip (trackIdx, sceneIdx);
        }
        else
        {
            if (sessionView.onLaunchClip) sessionView.onLaunchClip (trackIdx, sceneIdx);
        }
    });
}

void MainComponent::csLaunchScene (int sceneIdx)
{
    juce::MessageManager::callAsync ([this, sceneIdx] {
        if (sceneIdx < 0) return;
        if (sessionView.onLaunchScene) sessionView.onLaunchScene (sceneIdx);
    });
}

void MainComponent::csPauseClip (int trackIdx, int sceneIdx)
{
    juce::MessageManager::callAsync ([this, trackIdx, sceneIdx] {
        if (trackIdx < 0 || trackIdx >= (int)audioTracks.size()) return;
        if (sceneIdx < 0 || sceneIdx >= (int)clipGrid[trackIdx].size()) return;
        if (sessionView.onPauseClip) sessionView.onPauseClip (trackIdx, sceneIdx);
    });
}

void MainComponent::csSelectTrack (int trackIdx)
{
    juce::MessageManager::callAsync ([this, trackIdx] {
        if (trackIdx < 0 || trackIdx >= numActiveTracks.load(std::memory_order_relaxed)) return;
        if (sessionView.onSelectTrack) sessionView.onSelectTrack (trackIdx);
    });
}

void MainComponent::csTrackArmChanged (int trackIdx, bool armed)
{
    juce::MessageManager::callAsync ([this, trackIdx, armed] {
        if (trackIdx < 0 || trackIdx >= (int)audioTracks.size()) return;
        if (sessionView.onTrackArmChanged) sessionView.onTrackArmChanged (trackIdx, armed);
    });
}

void MainComponent::csTrackVolumeChanged (int trackIdx, float gain)
{
    if (trackIdx < 0 || trackIdx >= (int)audioTracks.size()) return;
    audioTracks[trackIdx]->gain.store (gain, std::memory_order_relaxed);
    juce::MessageManager::callAsync ([this, trackIdx, gain] {
        sessionView.setTrackVolumeFader (trackIdx, gain);
    });
}

void MainComponent::csMasterVolumeChanged (float gain)
{
    masterTrack.gain.store (gain, std::memory_order_relaxed);
    juce::MessageManager::callAsync ([this, gain] {
        sessionView.setMasterVolumeFader (gain);
    });
}

void MainComponent::handleIncomingMidiMessage(juce::MidiInput* source,
                                               const juce::MidiMessage& message)
{
    juce::ignoreUnused(source);

    // ── 5.1 Control Surface: consume messages meant for hardware controllers ───
    // Must be first — prevents controller MIDI from leaking into the note path.
    if (controlSurfaceManager.handleMidi(message))
        return;

    // ── 4.1 MPE: attempt to interpret as MPE channel-voice expression ─────────
    // Note On / Note Off are forwarded to midiCollector even when MPE is active
    // (instruments need them for voice allocation on the correct channel).
    TrackCommand::MpePayload mpePayload;
    if (mpeZone.processMidiMessage(message, mpePayload))
    {
        // Pure expression message — push to target track's commandQueue.
        // The note still plays; this only modulates an already-sounding voice.
        const int targetTrack = mpeTargetTrack.load(std::memory_order_relaxed);
        const int nTracks     = numActiveTracks.load(std::memory_order_relaxed);
        if (targetTrack >= 0 && targetTrack < nTracks)
        {
            TrackCommand cmd;
            cmd.type = TrackCommand::Type::MpeExpression;
            cmd.mpe  = mpePayload;
            audioTracks[targetTrack]->commandQueue.push(cmd);
        }
        // Do NOT forward to midiCollector — PB/CP/CC74 are handled above.
        return;
    }

    // Standard (non-expression) MIDI: deliver to the collector for the render thread.
    // This includes Note On/Off on MPE member channels — the instrument allocates
    // voices by channel so they receive their own expression stream.
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
    juce::String title = "LiBeDAW - " + projectName + (projectIsDirty ? " *" : "");

    if (auto* tlc = getTopLevelComponent())
    {
        tlc->setName (title);               // keeps JUCE component name in sync
        if (auto* peer = tlc->getPeer())
            peer->setTitle (title);         // pushes the title to the OS title bar
    }
}

void MainComponent::markDirty()
{
    if (!projectIsDirty)
    {
        projectIsDirty = true;
        updateWindowTitle();
    }
}

void MainComponent::recalculatePDC()
{
    // Message-thread only.  No lock needed: effect chains are read via the same
    // atomic pointer the render thread uses; we only read it here, never write.
    const int nTracks = numActiveTracks.load(std::memory_order_relaxed);
    if (nTracks == 0) return;

    // ── Step 1: measure per-track latency (sum of all effects in chain) ───────
    std::vector<int> trackLatency (nTracks, 0);
    int maxLatency = 0;

    for (int t = 0; t < nTracks; ++t)
    {
        if (auto* vec = audioTracks[t]->activeEffectChain.load(std::memory_order_acquire))
        {
            for (auto* effect : *vec)
                if (effect) trackLatency[t] += effect->getLatencySamples();
        }
        maxLatency = std::max(maxLatency, trackLatency[t]);
    }

    // ── Step 2: write per-track compensation delay atomically ─────────────────
    // Tracks with less latency than the worst-case track get delayed by the
    // difference.  The render thread reads pdcDelaySamples and routes through
    // pdcLine accordingly.
    for (int t = 0; t < nTracks; ++t)
    {
        const int comp = maxLatency - trackLatency[t];
        audioTracks[t]->pdcDelaySamples.store(comp, std::memory_order_release);
        audioTracks[t]->pdcLine.setDelay(comp);
    }
}

// ── Sidechain Routing (2.1) ───────────────────────────────────────────────────
void MainComponent::setSidechainSource(int targetTrackIdx, int sourceTrackIdx)
{
    // Message-thread only. Validate bounds.
    const int nTracks = numActiveTracks.load(std::memory_order_relaxed);
    if (targetTrackIdx < 0 || targetTrackIdx >= nTracks) return;
    // sourceTrackIdx == -1 means "clear sidechain" — always valid.
    if (sourceTrackIdx != -1 &&
        (sourceTrackIdx < 0 || sourceTrackIdx >= nTracks || sourceTrackIdx == targetTrackIdx))
        return;

    audioTracks[targetTrackIdx]->sidechainSourceTrack.store(sourceTrackIdx, std::memory_order_release);
    markDirty();
}

// ── Group Track Management (2.2) ──────────────────────────────────────────────
void MainComponent::addGroupTrack()
{
    const int newGroupIdx = numGroupTracks.load(std::memory_order_relaxed);
    if (newGroupIdx >= 8) return; // cap at 8 group buses

    // Allocate the new group Track object (heap, non-movable due to atomics)
    groupTracks.push_back(std::make_unique<Track>());
    numGroupTracks.store(newGroupIdx + 1, std::memory_order_release);

    // Allocate scratch buffer
    groupScratches[newGroupIdx].setSize(2, currentBufferSize > 0 ? currentBufferSize : 512);
    groupScratches[newGroupIdx].clear();

    // Notify UI — name the group "Group A", "B", …
    juce::String grpName = "Group ";
    grpName += (char)('A' + newGroupIdx);
    groupTrackNames.push_back(grpName);
    sessionView.addGroupTrackInline(newGroupIdx, grpName);
    markDirty();
}

void MainComponent::deleteGroupTrack(int groupIdx)
{
    const int currentCount = numGroupTracks.load(std::memory_order_relaxed);
    if (groupIdx < 0 || groupIdx >= currentCount) return;

    // ── Step 1: Atomically null the effect chain ──────────────────────────
    Track* deadTrack = groupTracks[groupIdx].get();
    if (auto* oldVec = deadTrack->activeEffectChain.exchange(nullptr, std::memory_order_acq_rel))
        deadTrack->effectVectorGarbageQueue.push(oldVec);

    // ── Step 2: Re-route any children back to master ──────────────────────
    const int nTracks = numActiveTracks.load(std::memory_order_relaxed);
    for (int t = 0; t < nTracks; ++t)
    {
        int cur = audioTracks[t]->groupBusIndex.load(std::memory_order_relaxed);
        if (cur == groupIdx)
            audioTracks[t]->groupBusIndex.store(-1, std::memory_order_release);
        else if (cur > groupIdx)
            audioTracks[t]->groupBusIndex.store(cur - 1, std::memory_order_release); // compact index
    }

    // ── Step 3: Decrement the visible count ──────────────────────────────
    numGroupTracks.fetch_sub(1, std::memory_order_release);

    // ── Step 4: Compact the scratch buffers ──────────────────────────────
    for (int i = groupIdx; i < currentCount - 1; ++i)
        groupScratches[i] = std::move(groupScratches[i + 1]);
    groupScratches[currentCount - 1].setSize(0, 0);

    // Remove name entry
    if (groupIdx < (int)groupTrackNames.size())
        groupTrackNames.erase(groupTrackNames.begin() + groupIdx);

    // ── Step 5: Extract and async-delete ─────────────────────────────────
    std::unique_ptr<Track> owned = std::move(groupTracks[groupIdx]);
    groupTracks.erase(groupTracks.begin() + groupIdx);
    Track* raw = owned.release();
    juce::MessageManager::callAsync([raw] { delete raw; });

    sessionView.deleteGroupTrackInline(groupIdx);
    refreshGroupBadges();
    markDirty();
}

void MainComponent::refreshGroupBadges()
{
    const int nTracks = numActiveTracks.load(std::memory_order_relaxed);
    const int nGroups = numGroupTracks.load(std::memory_order_relaxed);
    // A fixed palette of group colours
    static const juce::Colour kGroupColours[] = {
        juce::Colour(0xff44aa88), juce::Colour(0xff8844aa),
        juce::Colour(0xffaa8844), juce::Colour(0xff4488aa),
        juce::Colour(0xffaa4488), juce::Colour(0xff88aa44),
        juce::Colour(0xff4444aa), juce::Colour(0xffaa4444),
    };
    for (int t = 0; t < nTracks; ++t)
    {
        int gIdx = audioTracks[t]->groupBusIndex.load(std::memory_order_relaxed);
        juce::String gName;
        juce::Colour gCol = juce::Colour(0xff44aa88);
        if (gIdx >= 0 && gIdx < nGroups)
        {
            gName = (gIdx < (int)groupTrackNames.size()) ? groupTrackNames[gIdx]
                                                         : ("G" + juce::String(gIdx + 1));
            gCol  = kGroupColours[gIdx % 8];
        }
        sessionView.setTrackGroupBadge(t, gIdx, gName, gCol);
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

    // ── 5.2 Control Surface: register APC Mini driver ─────────────────────────────────────────────
    // Registered here (after loadDeviceSettings / setAudioChannels) so the ALSA
    // sequencer is fully initialised before we call MidiOutput::getAvailableDevices().
    controlSurfaceManager.addSurface (std::make_unique<ApcMiniDriver> (*this));
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

    topBar->onBpmChanged = [this] {
        markDirty();
        syncWarpRatiosToTempo();
    };

    topBar->onQuit = [this] {
        // Blank all hardware LEDs before the process exits.
        // shutdownControlSurfaces() destroys the ApcMiniDriver, whose
        // destructor sweeps all notes off via rawmidi + drain + 150ms sleep.
        // We call it here explicitly, before JUCE's shutdown tears things down.
        shutdownControlSurfaces();

        // Give the USB host controller time to deliver the blank packets
        // to the device (the 150ms sleep is inside the destructor, but we
        // add a small margin here to be safe).
        juce::Thread::sleep (50);

        juce::JUCEApplication::getInstance()->systemRequestedQuit();
    };
}

void MainComponent::syncUIToProject()
{
    // Clear current tracks and UI
    numActiveTracks.store(0, std::memory_order_relaxed);
    // Clear all existing tracks (instruments, effects, commands)
    for (int t = 0; t < (int)audioTracks.size(); ++t) {
        audioTracks[t]->gain.store(1.0f);
        audioTracks[t]->commandQueue.push({ TrackCommand::Type::StopPattern, nullptr, 0 });
        if (auto* oldVec = audioTracks[t]->activeEffectChain.exchange(nullptr, std::memory_order_acq_rel)) {
            audioTracks[t]->effectVectorGarbageQueue.push(oldVec);
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
                        clipGrid.push_back(std::vector<ClipData>());
                        trackInstruments.push_back("");
                        loadedFiles.push_back(juce::File{});
                        arrangementTracks.push_back({});
                    }
                    numActiveTracks.store(std::max(numActiveTracks.load(), tIdx + 1), std::memory_order_relaxed);
                    
                    juce::String name = trackNode.getProperty("name", "Track " + juce::String(tIdx + 1));
                    float gain = trackNode.getProperty("gain", 1.0f);
                    audioTracks[tIdx]->gain.store(gain);
                    int scSrc = trackNode.getProperty("sidechainSourceTrack", -1);
                    audioTracks[tIdx]->sidechainSourceTrack.store(scSrc, std::memory_order_relaxed);
                    int grpBus = trackNode.getProperty("groupBusIndex", -1);
                    audioTracks[tIdx]->groupBusIndex.store(grpBus, std::memory_order_relaxed);

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
                            newInst->setHostTrack(static_cast<void*>(audioTracks[tIdx].get()));
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
                        std::vector<EffectProcessor*>* newChain = new std::vector<EffectProcessor*>();
                        for (int e = 0; e < effectsNode.getNumChildren(); ++e) {
                            auto effectNode = effectsNode.getChild(e);
                            if (effectNode.hasType("Effect")) {
                                juce::String type = effectNode.getProperty("type", "");
                                if (auto effect = EffectFactory::create(type)) {
                                    effect->prepareToPlay(currentSampleRate);
                                    if (effectNode.getNumChildren() > 0) {
                                        effect->loadState(effectNode.getChild(0));
                                    }
                                    newChain->push_back(effect.release());
                                }
                            }
                        }
                        if (auto* oldVec = audioTracks[tIdx]->activeEffectChain.exchange(newChain, std::memory_order_acq_rel)) {
                            audioTracks[tIdx]->effectVectorGarbageQueue.push(oldVec);
                        }
                    }

                    // Clips
                    auto clipsNode = trackNode.getChildWithName("Clips");
                    if (clipsNode.isValid()) {
                        for (int c = 0; c < clipsNode.getNumChildren(); ++c) {
                            auto clipNode = clipsNode.getChild(c);
                            if (clipNode.hasType("Clip")) {
                                int sIdx = clipNode.getProperty("scene", -1);
                                if (sIdx >= 0) {
                                    // Grow data model and UI column to accommodate this scene index
                                    while ((int)clipGrid[tIdx].size() <= sIdx) {
                                        clipGrid[tIdx].emplace_back();
                                        sessionView.addSceneToTrack(tIdx);
                                    }
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

                                // ── Warp data (3.1) ─────────────────────────────────────────
                                aClip.audioFilePath = aNode.getProperty("audioFilePath", "");
                                aClip.clipBpm       = aNode.getProperty("clipBpm", 0.0);
                                aClip.warpEnabled   = (int)aNode.getProperty("warpEnabled", 0) != 0;
                                {
                                    juce::String mode = aNode.getProperty("warpMode", "Complex");
                                    if (mode == "Beats") aClip.warpMode = ArrangementClip::WarpMode::Beats;
                                    else if (mode == "Tones") aClip.warpMode = ArrangementClip::WarpMode::Tones;
                                    else aClip.warpMode = ArrangementClip::WarpMode::Complex;
                                }
                                auto markersNode = aNode.getChildWithName("WarpMarkers");
                                if (markersNode.isValid()) {
                                    aClip.warpMarkers.clear();
                                    for (int mi = 0; mi < markersNode.getNumChildren(); ++mi) {
                                        auto wmNode = markersNode.getChild(mi);
                                        WarpMarker wm;
                                        wm.sourcePositionSeconds = wmNode.getProperty("sourceSec",  0.0);
                                        wm.targetBeat            = wmNode.getProperty("targetBeat", 0.0);
                                        aClip.warpMarkers.push_back(wm);
                                    }
                                }

                                // ── Takes & Comp (3.2) ───────────────────────────────────────────
                                auto takesNode = aNode.getChildWithName("Takes");
                                if (takesNode.isValid())
                                {
                                    for (int ti = 0; ti < takesNode.getNumChildren(); ++ti)
                                    {
                                        auto tNode = takesNode.getChild(ti);
                                        Take take;
                                        take.audioFilePath    = tNode.getProperty("path",      "");
                                        take.recordedStartSec = tNode.getProperty("startSec",  0.0);
                                        take.lengthSec        = tNode.getProperty("lengthSec", 0.0);
                                        take.takeIndex        = tNode.getProperty("index",     ti);
                                        aClip.takes.push_back(take);
                                    }
                                    auto regNode = aNode.getChildWithName("CompRegions");
                                    if (regNode.isValid())
                                    {
                                        for (int ri = 0; ri < regNode.getNumChildren(); ++ri)
                                        {
                                            auto rNode = regNode.getChild(ri);
                                            CompRegion cr;
                                            cr.takeIndex      = rNode.getProperty("takeIndex",  0);
                                            cr.startBeat      = rNode.getProperty("startBeat",  0.0);
                                            cr.endBeat        = rNode.getProperty("endBeat",    0.0);
                                            cr.fadeInSamples  = rNode.getProperty("fadeInSmp",  480);
                                            cr.fadeOutSamples = rNode.getProperty("fadeOutSmp", 480);
                                            aClip.compRegions.push_back(cr);
                                        }
                                    }
                                }

                                arrangementTracks[tIdx].push_back(aClip);

                                // Kick off the AudioClipPlayer load if this clip is warp-enabled.
                                if (aClip.warpEnabled && aClip.audioFilePath.isNotEmpty()) {
                                    juce::File af(aClip.audioFilePath);
                                    if (af.existsAsFile())
                                        loadAudioClipForTrack(tIdx, af);
                                }
                                // Kick off the CompPlayer load if this clip has takes (3.2).
                                if (!aClip.takes.empty())
                                    loadCompPlayerForTrack(tIdx);
                            }
                        }
                    }


                }
            }
        }
    }

    // Return Tracks
    if (numReturnTracks.load() > 0) {
        auto& returnTrackA = *returnTracks[0];
        if (auto* oldVec = returnTrackA.activeEffectChain.exchange(nullptr, std::memory_order_acq_rel)) {
            returnTrackA.effectVectorGarbageQueue.push(oldVec);
        }
        auto returnTrackNode = pTree.getChildWithName("ReturnTrackA");
        if (returnTrackNode.isValid()) {
            float gain = returnTrackNode.getProperty("gain", 1.0f);
            returnTrackA.gain.store(gain, std::memory_order_relaxed);
            
            auto returnEffectsNode = returnTrackNode.getChildWithName("Effects");
            if (returnEffectsNode.isValid()) {
                std::vector<EffectProcessor*>* newChain = new std::vector<EffectProcessor*>();
                for (int e = 0; e < returnEffectsNode.getNumChildren(); ++e) {
                    auto effectNode = returnEffectsNode.getChild(e);
                    if (effectNode.hasType("Effect")) {
                        juce::String type = effectNode.getProperty("type", "");
                        if (auto effect = EffectFactory::create(type)) {
                            effect->prepareToPlay(currentSampleRate);
                            if (effectNode.getNumChildren() > 0) {
                                effect->loadState(effectNode.getChild(0));
                            }
                            newChain->push_back(effect.release());
                        }
                    }
                }
                if (auto* oldVec = returnTrackA.activeEffectChain.exchange(newChain, std::memory_order_acq_rel)) {
                    returnTrackA.effectVectorGarbageQueue.push(oldVec);
                }
            }
        }
    }

    // ── Group Tracks ─────────────────────────────────────────────────────────
    {
        auto groupsNode = pTree.getChildWithName("GroupTracks");
        if (groupsNode.isValid())
        {
            for (int g = 0; g < groupsNode.getNumChildren(); ++g)
            {
                auto gNode = groupsNode.getChild(g);
                if (!gNode.hasType("Group")) continue;

                addGroupTrack(); // creates engine Track + UI column + increments numGroupTracks
                int gIdx = numGroupTracks.load(std::memory_order_relaxed) - 1;

                // Override the default name with the saved name
                juce::String savedName = gNode.getProperty("name", groupTrackNames[gIdx]);
                groupTrackNames[gIdx] = savedName;
                // Update the UI column header name
                if (gIdx < (int)sessionView.groupColumns.size())
                    sessionView.groupColumns[gIdx]->trackName = savedName;

                bool  savedFolded = gNode.getProperty("folded", false);
                float gain = gNode.getProperty("gain", 1.0f);
                groupTracks[gIdx]->gain.store(gain, std::memory_order_relaxed);
                if (savedFolded) sessionView.setGroupFolded(gIdx, true);

                auto gEffects = gNode.getChildWithName("Effects");
                if (gEffects.isValid())
                {
                    auto* newChain = new std::vector<EffectProcessor*>();
                    for (int e = 0; e < gEffects.getNumChildren(); ++e)
                    {
                        auto eNode = gEffects.getChild(e);
                        if (!eNode.hasType("Effect")) continue;
                        juce::String type = eNode.getProperty("type", "");
                        if (auto eff = EffectFactory::create(type))
                        {
                            eff->prepareToPlay(currentSampleRate);
                            if (eNode.getNumChildren() > 0) eff->loadState(eNode.getChild(0));
                            newChain->push_back(eff.release());
                        }
                    }
                    if (auto* old = groupTracks[gIdx]->activeEffectChain.exchange(
                            newChain, std::memory_order_acq_rel))
                        groupTracks[gIdx]->effectVectorGarbageQueue.push(old);
                }
            }
        }
        // Restore groupBusIndex per track: push to UI display order
        {
            int nT = numActiveTracks.load(std::memory_order_relaxed);
            int nG = numGroupTracks.load(std::memory_order_relaxed);
            for (int t = 0; t < nT; ++t)
            {
                int gIdx = audioTracks[t]->groupBusIndex.load(std::memory_order_relaxed);
                if (gIdx >= 0 && gIdx < nG)
                    sessionView.setTrackParentGroup(t, gIdx);
            }
        }
        refreshGroupBadges();
    }

    syncArrangementFromSession();

    // ── 5.2 Control Surface: after project load, broadcast HasClip state ──────
    // notifyLayout() blanks all LEDs and then performs a full sync based on the
    // now-populated clipGrid.
    controlSurfaceManager.notifyLayout();
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
        trackNode.setProperty("sidechainSourceTrack", audioTracks[t]->sidechainSourceTrack.load(std::memory_order_relaxed), nullptr);
        trackNode.setProperty("groupBusIndex",        audioTracks[t]->groupBusIndex.load(std::memory_order_relaxed),        nullptr);

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
        for (int s = 0; s < (int)clipGrid[t].size(); ++s) {
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

            // ── Warp data (3.1) ───────────────────────────────────────────
            if (!aClip.audioFilePath.isEmpty())
                aClipNode.setProperty("audioFilePath", aClip.audioFilePath, nullptr);
            if (aClip.clipBpm > 0.0)
                aClipNode.setProperty("clipBpm", aClip.clipBpm, nullptr);
            aClipNode.setProperty("warpEnabled", (int)aClip.warpEnabled, nullptr);
            {
                const char* modeStr = "Complex";
                if (aClip.warpMode == ArrangementClip::WarpMode::Beats) modeStr = "Beats";
                else if (aClip.warpMode == ArrangementClip::WarpMode::Tones) modeStr = "Tones";
                aClipNode.setProperty("warpMode", modeStr, nullptr);
            }
            if (!aClip.warpMarkers.empty())
            {
                juce::ValueTree markersNode("WarpMarkers");
                for (const auto& wm : aClip.warpMarkers)
                {
                    juce::ValueTree wmNode("WarpMarker");
                    wmNode.setProperty("sourceSec",  wm.sourcePositionSeconds, nullptr);
                    wmNode.setProperty("targetBeat", wm.targetBeat,            nullptr);
                    markersNode.addChild(wmNode, -1, nullptr);
                }
                aClipNode.addChild(markersNode, -1, nullptr);
            }

            // ── Takes & Comp (3.2) ────────────────────────────────────────────────
            if (!aClip.takes.empty())
            {
                juce::ValueTree takesNode("Takes");
                for (const auto& take : aClip.takes)
                {
                    juce::ValueTree tNode("Take");
                    tNode.setProperty("path",      take.audioFilePath,    nullptr);
                    tNode.setProperty("startSec",  take.recordedStartSec, nullptr);
                    tNode.setProperty("lengthSec", take.lengthSec,        nullptr);
                    tNode.setProperty("index",     take.takeIndex,        nullptr);
                    takesNode.addChild(tNode, -1, nullptr);
                }
                aClipNode.addChild(takesNode, -1, nullptr);

                juce::ValueTree regionsNode("CompRegions");
                for (const auto& cr : aClip.compRegions)
                {
                    juce::ValueTree rNode("Region");
                    rNode.setProperty("takeIndex",  cr.takeIndex,      nullptr);
                    rNode.setProperty("startBeat",  cr.startBeat,      nullptr);
                    rNode.setProperty("endBeat",    cr.endBeat,        nullptr);
                    rNode.setProperty("fadeInSmp",  cr.fadeInSamples,  nullptr);
                    rNode.setProperty("fadeOutSmp", cr.fadeOutSamples, nullptr);
                    regionsNode.addChild(rNode, -1, nullptr);
                }
                aClipNode.addChild(regionsNode, -1, nullptr);
            }

            arrangementNode.addChild(aClipNode, -1, nullptr);
        }
        if (arrangementNode.getNumChildren() > 0)
            trackNode.addChild(arrangementNode, -1, nullptr);
            
        // Effects
        juce::ValueTree effectsNode("Effects");
        if (auto* vec = audioTracks[t]->activeEffectChain.load(std::memory_order_acquire)) {
            for (auto* effect : *vec) {
                if (effect) {
                    juce::ValueTree effectNode("Effect");
                    effectNode.setProperty("type", effect->getName(), nullptr);
                    effectNode.addChild(effect->saveState(), -1, nullptr);
                    effectsNode.addChild(effectNode, -1, nullptr);
                }
            }
        }
        if (effectsNode.getNumChildren() > 0)
            trackNode.addChild(effectsNode, -1, nullptr);

        tracksTree.addChild(trackNode, -1, nullptr);
    }

    // Return Tracks (just ReturnTrackA for backwards compatibility)
    if (numReturnTracks.load() > 0) {
        juce::ValueTree returnTrackNode("ReturnTrackA");
        returnTrackNode.setProperty("gain", returnTracks[0]->gain.load(std::memory_order_relaxed), nullptr);
        juce::ValueTree returnEffectsNode("Effects");
        if (auto* vec = returnTracks[0]->activeEffectChain.load(std::memory_order_acquire)) {
            for (auto* effect : *vec) {
                if (effect) {
                    juce::ValueTree effectNode("Effect");
                    effectNode.setProperty("type", effect->getName(), nullptr);
                    effectNode.addChild(effect->saveState(), -1, nullptr);
                    returnEffectsNode.addChild(effectNode, -1, nullptr);
                }
            }
        }
        if (returnEffectsNode.getNumChildren() > 0)
            returnTrackNode.addChild(returnEffectsNode, -1, nullptr);
        pTree.addChild(returnTrackNode, -1, nullptr);
    }

    // ── Group Tracks ─────────────────────────────────────────────────────────
    {
        juce::ValueTree groupsNode("GroupTracks");
        const int nGroups = numGroupTracks.load(std::memory_order_relaxed);
        for (int g = 0; g < nGroups; ++g)
        {
            juce::ValueTree gNode("Group");
            juce::String gName = (g < (int)groupTrackNames.size()) ? groupTrackNames[g]
                                                                    : ("Group " + juce::String(g + 1));
            gNode.setProperty("name", gName, nullptr);
            gNode.setProperty("gain", groupTracks[g]->gain.load(std::memory_order_relaxed), nullptr);
            gNode.setProperty("folded", (bool)sessionView.gridContent.groupFolded[g], nullptr);
            // Effect chain
            juce::ValueTree gEffects("Effects");
            if (auto* vec = groupTracks[g]->activeEffectChain.load(std::memory_order_acquire)) {
                for (auto* effect : *vec) {
                    if (effect) {
                        juce::ValueTree eNode("Effect");
                        eNode.setProperty("type", effect->getName(), nullptr);
                        eNode.addChild(effect->saveState(), -1, nullptr);
                        gEffects.addChild(eNode, -1, nullptr);
                    }
                }
            }
            if (gEffects.getNumChildren() > 0)
                gNode.addChild(gEffects, -1, nullptr);
            groupsNode.addChild(gNode, -1, nullptr);
        }
        if (groupsNode.getNumChildren() > 0)
            pTree.addChild(groupsNode, -1, nullptr);
    }
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

// ─────────────────────────────────────────────────────────────────────────────
// loadAudioClipForTrack (3.1) — message thread
// Creates/reuses an AudioClipPlayer for trackIdx and loads the given file into
// it.  The clip player will be activated (clipPlayerActive = true) once the
// file is successfully loaded so the render thread can start using it.
// ─────────────────────────────────────────────────────────────────────────────
void MainComponent::loadAudioClipForTrack(int trackIdx, const juce::File& file)
{
    if (trackIdx < 0 || trackIdx >= (int)audioTracks.size()) return;
    if (!file.existsAsFile()) return;

    auto* track = audioTracks[trackIdx].get();

    // Create a new player if needed (or reuse the existing one).
    if (track->clipPlayer == nullptr)
        track->clipPlayer = std::make_unique<AudioClipPlayer>();

    // Deactivate while loading.
    track->clipPlayerActive.store(false, std::memory_order_release);

    // Load on a background thread (file I/O + pre-roll can take ~50 ms).
    juce::Thread::launch([this, trackIdx, file] {
        auto* t = audioTracks[trackIdx].get();
        if (t == nullptr || t->clipPlayer == nullptr) return;

        bool ok = t->clipPlayer->load(file, currentSampleRate,
                                      juce::jmax(currentBufferSize, 512));
        if (ok)
        {
            // Sync the stretch ratio from the current BPM before activating.
            juce::MessageManager::callAsync([this, trackIdx] {
                if (trackIdx >= (int)audioTracks.size()) return;
                auto* tr = audioTracks[trackIdx].get();
                if (!tr || !tr->clipPlayer) return;
                // Find the clip's stored BPM from arrangementTracks.
                if (trackIdx < (int)arrangementTracks.size()) {
                    for (const auto& clip : arrangementTracks[trackIdx]) {
                        if (clip.warpEnabled && clip.clipBpm > 0.0 && !clip.audioFilePath.isEmpty()) {
                            tr->clipPlayer->setStretchRatio(transportClock.getBpm() / clip.clipBpm);
                            tr->clipPlayer->setWarpMarkers(clip.warpMarkers);
                            break;
                        }
                    }
                }
                tr->clipPlayerActive.store(true, std::memory_order_release);
                DBG("AudioClipPlayer activated for track " << trackIdx);
            });
        }
        else
        {
            DBG("AudioClipPlayer failed to load: " << file.getFullPathName());
        }
    });
}

// ─────────────────────────────────────────────────────────────────────────────
// syncWarpRatiosToTempo (3.1) — message thread
// Called whenever the global BPM changes.  Updates every active AudioClipPlayer
// whose clip has a known clipBpm so that its stretch ratio tracks the new tempo.
// ─────────────────────────────────────────────────────────────────────────────
void MainComponent::syncWarpRatiosToTempo()
{
    const double bpm = transportClock.getBpm();
    const int    n   = numActiveTracks.load(std::memory_order_relaxed);

    for (int t = 0; t < n; ++t)
    {
        auto* track = audioTracks[t].get();
        if (!track) continue;
        if (!track->clipPlayerActive.load(std::memory_order_acquire)) continue;
        if (!track->clipPlayer || !track->clipPlayer->isLoaded()) continue;

        // Find this track's warp-enabled clip to get the original BPM.
        if (t < (int)arrangementTracks.size())
        {
            for (const auto& clip : arrangementTracks[t])
            {
                if (clip.warpEnabled && clip.clipBpm > 0.0 && !clip.audioFilePath.isEmpty())
                {
                    track->clipPlayer->setStretchRatio(bpm / clip.clipBpm);
                    break;
                }
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// registerNewTake (3.2) — message thread
// Called by RecordingThread (via callAsync) after each WAV is finalised.
// Appends the new Take to the first arrangement clip on trackIdx.
// Enforces the 8-take cap; reloads the CompPlayer.
// ─────────────────────────────────────────────────────────────────────────────
void MainComponent::registerNewTake(int trackIdx, const juce::File& file, int64_t startSample)
{
    if (trackIdx < 0 || trackIdx >= (int)arrangementTracks.size()) return;
    if (!file.existsAsFile()) return;

    auto& clips = arrangementTracks[trackIdx];
    if (clips.empty())
    {
        ArrangementClip newClip;
        newClip.trackIndex       = trackIdx;
        newClip.startBar         = 1.0;
        newClip.lengthBars       = 4.0;
        newClip.data.hasClip     = true;
        newClip.data.name        = "Recorded";
        clips.push_back(newClip);
    }
    auto& clip = clips[0];

    // ── 8-take cap ────────────────────────────────────────────────────────────
    if ((int)clip.takes.size() >= Take::kMaxTakesPerClip)
    {
        DBG("registerNewTake: cap (" + juce::String(Take::kMaxTakesPerClip) + ") reached, dropping oldest take.");
        clip.takes.erase(clip.takes.begin());
        for (int i = 0; i < (int)clip.takes.size(); ++i) clip.takes[i].takeIndex = i;
        clip.compRegions.erase(
            std::remove_if(clip.compRegions.begin(), clip.compRegions.end(),
                [](const CompRegion& r) { return r.takeIndex == 0; }),
            clip.compRegions.end());
        for (auto& r : clip.compRegions) r.takeIndex = juce::jmax(0, r.takeIndex - 1);
    }

    // Build Take entry
    juce::AudioFormatManager fmt; fmt.registerBasicFormats();
    double dur = 0.0;
    if (auto* reader = fmt.createReaderFor(file)) { dur = static_cast<double>(reader->lengthInSamples) / reader->sampleRate; delete reader; }

    Take tk;
    tk.audioFilePath    = file.getFullPathName();
    tk.recordedStartSec = static_cast<double>(startSample) / currentSampleRate;
    tk.lengthSec        = dur;
    tk.takeIndex        = static_cast<int>(clip.takes.size());
    clip.takes.push_back(tk);

    // Default full-clip comp region for the very first take
    if (clip.compRegions.empty())
    {
        CompRegion def;
        def.takeIndex = 0;
        def.startBeat = 0.0;
        def.endBeat   = clip.lengthBars * 4.0;
        clip.compRegions.push_back(def);
    }

    loadCompPlayerForTrack(trackIdx);
}

// ─────────────────────────────────────────────────────────────────────────────
// loadCompPlayerForTrack (3.2) — message thread
// ─────────────────────────────────────────────────────────────────────────────
void MainComponent::loadCompPlayerForTrack(int trackIdx)
{
    if (trackIdx < 0 || trackIdx >= (int)audioTracks.size()) return;
    if (trackIdx >= (int)arrangementTracks.size()) return;
    auto& clips = arrangementTracks[trackIdx];
    if (clips.empty() || clips[0].takes.empty()) return;

    auto* track = audioTracks[trackIdx].get();
    if (!track) return;

    track->compPlayerActive.store(false, std::memory_order_release);
    if (!track->compPlayer) track->compPlayer = std::make_unique<CompPlayer>();

    track->compPlayer->setTakes(clips[0].takes, currentSampleRate, juce::jmax(currentBufferSize, 512));
    track->compPlayer->setCompRegions(clips[0].compRegions);

    syncArrangementFromSession();
    DBG("loadCompPlayerForTrack: " + juce::String(clips[0].takes.size()) + " takes loaded for track " + juce::String(trackIdx));
}

// ─────────────────────────────────────────────────────────────────────────────
// onCompRegionSwiped (3.2) — message thread
// Invoked by TakeLaneOverlay after the user completes a swipe gesture.
// ─────────────────────────────────────────────────────────────────────────────
void MainComponent::onCompRegionSwiped(int trackIdx, int clipIdx, int takeIdx,
                                       double startBeat, double endBeat)
{
    if (trackIdx < 0 || trackIdx >= (int)arrangementTracks.size()) return;
    auto& clips = arrangementTracks[trackIdx];
    if (clipIdx < 0 || clipIdx >= (int)clips.size()) return;
    auto& clip = clips[clipIdx];
    if (clip.takes.empty()) return;
    takeIdx = juce::jlimit(0, (int)clip.takes.size() - 1, takeIdx);

    // Snap to 1/16 bar
    const double snap = 4.0 / 16.0;
    startBeat = std::round(startBeat / snap) * snap;
    endBeat   = std::round(endBeat   / snap) * snap;
    if (endBeat <= startBeat) endBeat = startBeat + snap;

    // Rebuild regions
    std::vector<CompRegion> newRegions;
    for (const auto& r : clip.compRegions)
    {
        if (r.endBeat <= startBeat)                                         newRegions.push_back(r);
        else if (r.startBeat < startBeat && r.endBeat > startBeat)         { auto h = r; h.endBeat = startBeat; newRegions.push_back(h); }
        else if (r.startBeat >= startBeat && r.endBeat <= endBeat)         { /* drop */ }
        else if (r.startBeat < endBeat && r.endBeat > endBeat)             { auto tl = r; tl.startBeat = endBeat; newRegions.push_back(tl); }
        else if (r.startBeat >= endBeat)                                    newRegions.push_back(r);
    }
    CompRegion sw; sw.takeIndex = takeIdx; sw.startBeat = startBeat; sw.endBeat = endBeat;
    sw.fadeInSamples = sw.fadeOutSamples = 480;
    newRegions.push_back(sw);
    std::sort(newRegions.begin(), newRegions.end(),
        [](const CompRegion& a, const CompRegion& b){ return a.startBeat < b.startBeat; });
    clip.compRegions = newRegions;

    if (trackIdx < (int)audioTracks.size() && audioTracks[trackIdx]->compPlayer)
        audioTracks[trackIdx]->compPlayer->setCompRegions(clip.compRegions);

    syncArrangementFromSession();
    markDirty();

    // Background zero-crossing search at each new boundary
    for (int ri = 0; ri < (int)clip.compRegions.size() - 1; ++ri)
    {
        if (clip.compRegions[ri].takeIndex == clip.compRegions[ri+1].takeIndex) continue;
        juce::String outPath = clip.takes[juce::jlimit(0,(int)clip.takes.size()-1, clip.compRegions[ri].takeIndex)].audioFilePath;
        juce::String inPath  = clip.takes[juce::jlimit(0,(int)clip.takes.size()-1, clip.compRegions[ri+1].takeIndex)].audioFilePath;
        double bnd = clip.compRegions[ri].endBeat;
        double spb = transportClock.getSamplesPerBeat();
        juce::Thread::launch([this, trackIdx, clipIdx, ri, bnd, outPath, inPath, spb]()
        {
            const int kWin = 512;
            int bestOut = 480, bestIn = 480;
            juce::AudioFormatManager f; f.registerBasicFormats();
            int64_t outSmp = static_cast<int64_t>(bnd * spb);
            if (auto* rd = f.createReaderFor(juce::File(outPath))) {
                int64_t ss = std::max((int64_t)0, outSmp - (int64_t)kWin);
                int64_t avail = rd->lengthInSamples - ss;
                int sl = (int)std::min((int64_t)(kWin*2), avail);
                if (sl > 1) { juce::AudioBuffer<float> b(1,sl); rd->read(&b,0,sl,ss,true,false); const float* s=b.getReadPointer(0); int c=(int)(outSmp-ss); for(int d=0;d<kWin;++d){ if(c+d<sl-1&&s[c+d]*s[c+d+1]<=0.f){bestOut=juce::jmax(1,d);break;} if(c-d>=1&&s[c-d]*s[c-d-1]<=0.f){bestOut=juce::jmax(1,d);break;} } } delete rd;
            }
            if (auto* rd = f.createReaderFor(juce::File(inPath))) {
                int sl=(int)std::min((int64_t)1024, (int64_t)rd->lengthInSamples); if(sl>1){juce::AudioBuffer<float> b(1,sl);rd->read(&b,0,sl,0,true,false);const float*s=b.getReadPointer(0);for(int i=0;i<sl-1;++i)if(s[i]*s[i+1]<=0.f){bestIn=juce::jmax(1,i);break;}} delete rd;
            }
            juce::MessageManager::callAsync([this,trackIdx,clipIdx,ri,bestOut,bestIn](){
                if(trackIdx>=(int)arrangementTracks.size()) return;
                auto& c=arrangementTracks[trackIdx]; if(clipIdx>=(int)c.size()) return;
                auto& rg=c[clipIdx].compRegions; if(ri>=(int)rg.size()) return;
                rg[ri].fadeOutSamples=bestOut; if(ri+1<(int)rg.size()) rg[ri+1].fadeInSamples=bestIn;
                if(trackIdx<(int)audioTracks.size()&&audioTracks[trackIdx]->compPlayer) audioTracks[trackIdx]->compPlayer->setCompRegions(rg);
                syncArrangementFromSession();
            });
        });
    }
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
    if (trackIdx >= 2000) {
        // Group bus track
        int grpIdx = trackIdx - 2000;
        if (grpIdx >= 0 && grpIdx < numGroupTracks.load())
            track = groupTracks[grpIdx].get();
    } else if (trackIdx >= 1000) {
        int retIdx = trackIdx - 1000;
        if (retIdx >= 0 && retIdx < numReturnTracks.load())
            track = returnTracks[retIdx].get();
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
    if (auto* vec = track->activeEffectChain.load(std::memory_order_acquire)) {
        for (size_t i = 0; i < vec->size(); ++i) {
            auto* effect = (*vec)[i];
            if (effect) {
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
                    std::vector<EffectProcessor*>* newChain = new std::vector<EffectProcessor*>();
                    if (auto* oldVec = track->activeEffectChain.load(std::memory_order_acquire)) {
                        *newChain = *oldVec;
                        if (i < newChain->size()) {
                            auto* oldEff = (*newChain)[i];
                            newChain->erase(newChain->begin() + i);
                            track->effectGarbageQueue.push(oldEff);
                        }
                    }
                    if (auto* oldVec = track->activeEffectChain.exchange(newChain, std::memory_order_acq_rel)) {
                        track->effectVectorGarbageQueue.push(oldVec);
                    }
                    track->refreshAutomationRegistry();
                    showDeviceEditorForTrack(trackIdx);
                };

                // ── Sidechain editor wiring (2.1) ───────────────────────────────
                // If this is a CompressorEditor, populate the sidechain source
                // ComboBox with the current track list and wire its callback back
                // to the engine-side setSidechainSource().
                if (auto* compEditor = dynamic_cast<CompressorEditor*>(wrapper->content.get()))
                {
                    // Wire the combo → engine
                    auto* compEffect = dynamic_cast<CompressorEffect*>(effect);
                    if (compEffect)
                    {
                        // Store self-track index on the effect's atomic so the editor
                        // can reflect it correctly (if already set).
                        // Also wire onSidechainSourceChanged → engine + atomic.
                        compEffect->onSidechainSourceChanged = [this, compEffect, trackIdx](int srcIdx) {
                            compEffect->sidechainSourceIndex.store(srcIdx, std::memory_order_relaxed);
                            setSidechainSource(trackIdx, srcIdx);
                        };
                    }

                    // Populate combo with current track names
                    juce::StringArray names;
                    const int nT = numActiveTracks.load(std::memory_order_relaxed);
                    for (int ti = 0; ti < nT; ++ti)
                    {
                        juce::String tName = "Track " + juce::String(ti + 1);
                        if (ti < (int)sessionView.gridContent.columns.size())
                            tName = sessionView.gridContent.columns[ti]->header.trackName;
                        names.add(tName);
                    }
                    // selfTrackIdx: for return tracks we use -1 (no self-exclusion needed)
                    int selfIdx = (trackIdx >= 0 && trackIdx < nT) ? trackIdx : -1;
                    compEditor->refreshSidechainSources(names, selfIdx);
                }

                deviceView.addEditor(std::move(wrapper));
            }
        }
    }

    std::unordered_map<juce::String, DeviceView::AutomationParamInfo> autoInfo;
    if (trackIdx >= 0 && trackIdx < (int)audioTracks.size() && selectedSceneIndex >= 0 && selectedSceneIndex < (int)clipGrid[selectedTrackIndex].size()) {
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
        syncArrangementFromSession();
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
