#include "AudioClipPlayer.h"

// ─────────────────────────────────────────────────────────────────────────────
// AudioClipPlayer — Implementation
// ─────────────────────────────────────────────────────────────────────────────

AudioClipPlayer::AudioClipPlayer()
    : decoderThread_(*this)
{
    formatManager = std::make_unique<juce::AudioFormatManager>();
    formatManager->registerBasicFormats();
}

AudioClipPlayer::~AudioClipPlayer()
{
    unload();
}

// ─────────────────────────────────────────────────────────────────────────────
// load() — called on the message thread
// ─────────────────────────────────────────────────────────────────────────────
bool AudioClipPlayer::load(const juce::File& file, double sampleRate, int maxBlockSize)
{
    // Stop decoder thread while we reconfigure.
    if (decoderThread_.isThreadRunning())
    {
        loaded_.store(false, std::memory_order_release);
        decoderThread_.stopThread(500);
    }

    sampleRate_   = sampleRate;
    maxBlockSize_ = maxBlockSize;

    // Open the audio file.
    auto* reader = formatManager->createReaderFor(file);
    if (reader == nullptr)
    {
        DBG("AudioClipPlayer::load — could not open: " << file.getFullPathName());
        return false;
    }

    numChannels_  = juce::jmin(2, (int)reader->numChannels);
    totalSamples  = reader->lengthInSamples;
    readerPosition = 0;

    readerSource = std::make_unique<juce::AudioFormatReaderSource>(reader, true /*owns reader*/);
    readerSource->setNextReadPosition(0);

    // Allocate ring buffers (in load, never in fillBlock).
    inputRing.setSize(numChannels_,  kDecoderRingCapacity, false, true, false);
    outputRing.setSize(numChannels_, kOutputRingCapacity,  false, true, false);
    inputFifo.setTotalSize(kDecoderRingCapacity);
    inputFifo.reset();
    outputFifo.setTotalSize(kOutputRingCapacity);
    outputFifo.reset();

    decodeScratch.setSize(numChannels_,  juce::jmax(maxBlockSize_, 2048), false, true, false);
    retrieveScratch.setSize(numChannels_, juce::jmax(maxBlockSize_, 2048), false, true, false);

    // Build the RubberBand stretcher in real-time mode with the R3 (Phase Vocoder) engine.
    using RBS = RubberBand::RubberBandStretcher;
    RBS::Options opts = RBS::OptionProcessRealTime
                      | RBS::OptionEngineFiner        // R3 engine (high quality)
                      | RBS::OptionPitchHighConsistency;

    stretcher = std::make_unique<RBS>(
        static_cast<size_t>(sampleRate_),
        static_cast<size_t>(numChannels_),
        opts,
        stretchRatio.load(std::memory_order_relaxed),
        1.0 /*pitch scale*/);

    stretcher->setMaxProcessSize(static_cast<size_t>(juce::jmax(maxBlockSize_, 2048)));
    stretcherLatency = static_cast<int>(stretcher->getLatency());

    // Pre-roll: feed silent samples to flush the stretcher's internal latency.
    {
        juce::AudioBuffer<float> silence(numChannels_,
                                         juce::jmax(stretcherLatency, maxBlockSize_));
        silence.clear();
        const float* silencePtr[2] = { silence.getReadPointer(0),
                                       numChannels_ > 1 ? silence.getReadPointer(1) : silence.getReadPointer(0) };
        stretcher->process(silencePtr, static_cast<size_t>(silence.getNumSamples()), false);
    }

    loaded_.store(true, std::memory_order_release);
    seekPending_.store(false, std::memory_order_relaxed);

    // Start decoder thread.
    decoderThread_.startThread(juce::Thread::Priority::normal);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// unload() — message thread
// ─────────────────────────────────────────────────────────────────────────────
void AudioClipPlayer::unload()
{
    loaded_.store(false, std::memory_order_release);
    if (decoderThread_.isThreadRunning())
        decoderThread_.stopThread(500);

    readerSource.reset();
    stretcher.reset();
    stretcherLatency = 0;
    totalSamples = 0;
    readerPosition = 0;
}

bool AudioClipPlayer::isLoaded() const
{
    return loaded_.load(std::memory_order_acquire);
}

double AudioClipPlayer::getLengthSeconds() const
{
    if (sampleRate_ <= 0.0 || totalSamples <= 0) return 0.0;
    return static_cast<double>(totalSamples) / sampleRate_;
}

// ─────────────────────────────────────────────────────────────────────────────
// setStretchRatio / getStretchRatio
// ─────────────────────────────────────────────────────────────────────────────
void AudioClipPlayer::setStretchRatio(double ratio)
{
    ratio = juce::jlimit(0.1, 10.0, ratio);
    stretchRatio.store(ratio, std::memory_order_relaxed);
}

double AudioClipPlayer::getStretchRatio() const
{
    return stretchRatio.load(std::memory_order_relaxed);
}

// ─────────────────────────────────────────────────────────────────────────────
// setWarpMarkers — message thread
// ─────────────────────────────────────────────────────────────────────────────
void AudioClipPlayer::setWarpMarkers(const std::vector<WarpMarker>& markers)
{
    juce::SpinLock::ScopedLockType sl(markerLock_);
    warpMarkers_ = markers;
}

// ─────────────────────────────────────────────────────────────────────────────
// seek — message thread
// ─────────────────────────────────────────────────────────────────────────────
void AudioClipPlayer::seek(double clipBeat, double samplesPerBeat)
{
    double ratio = stretchRatio.load(std::memory_order_relaxed);
    // Convert beat position to a position in the raw (un-stretched) file.
    // clipBeat * samplesPerBeat gives the stretched sample position.
    // Dividing by ratio gives the corresponding raw sample position.
    juce::int64 rawSample = 0;
    if (ratio > 0.0 && samplesPerBeat > 0.0)
        rawSample = static_cast<juce::int64>((clipBeat * samplesPerBeat) / ratio);
    rawSample = juce::jlimit((juce::int64)0, totalSamples, rawSample);
    seekSample_.store(rawSample, std::memory_order_release);
    seekPending_.store(true, std::memory_order_release);
}

// ─────────────────────────────────────────────────────────────────────────────
// ratioAtBeat — compute local stretch ratio using warp markers
// May be called from decoder thread (under markerLock_ held externally) or
// render thread (we take a try-lock; fall back to global ratio if locked).
// ─────────────────────────────────────────────────────────────────────────────
double AudioClipPlayer::ratioAtBeat(double clipBeat) const
{
    const double globalRatio = stretchRatio.load(std::memory_order_relaxed);

    juce::SpinLock::ScopedTryLockType sl(const_cast<juce::SpinLock&>(markerLock_));
    if (!sl.isLocked() || warpMarkers_.size() < 2)
        return globalRatio;

    // Find the segment containing clipBeat.
    for (size_t i = 0; i + 1 < warpMarkers_.size(); ++i)
    {
        const auto& a = warpMarkers_[i];
        const auto& b = warpMarkers_[i + 1];
        if (clipBeat >= a.targetBeat && clipBeat < b.targetBeat)
        {
            double targetDelta = b.targetBeat - a.targetBeat;   // beats
            double sourceDelta = b.sourcePositionSeconds - a.sourcePositionSeconds; // seconds
            if (sourceDelta <= 0.0 || targetDelta <= 0.0)
                return globalRatio;
            // ratio = stretchedDuration / rawDuration  (both in seconds, same unit)
            // stretchedSeconds = targetDelta / beatsPerSec = targetDelta / (BPM/60)
            // But we don't have BPM here — use the global ratio as the baseline and
            // adjust segment-locally by the ratio of target/source durations.
            // This is equivalent to: local_ratio = (targetDelta / beatsPerSec) / sourceDelta
            // Since beatsPerSec = globalRatio * (sourceBeatDuration)... we simplify:
            // local_ratio = (targetDelta * sourceDeltaSeconds) / (targetDeltaSeconds * sourceDelta)
            // Simplest correct form: local_ratio = targetDelta / (sourceDelta * beatsPerSecond)
            // We compute local_ratio relative to the global ratio so we don't need BPM:
            // local_ratio / global_ratio = targetStretchedSec / rawSec
            // => local_ratio = global_ratio * (targetStretchedSec / rawSec)
            // targetStretchedSec = targetDelta / (BPM/60) ... still needs BPM.
            //
            // SIMPLIFICATION: for warp-marker-driven clips, just use the per-segment
            // source/target ratio directly as:
            //   local_ratio = targetDeltaBeats / (sourceDelta * samplesPerBeat / sampleRate)
            // We return a ratio relative to source audio seconds:
            //   stretched_seconds = targetDelta beats * (sampleRate_ / samplesPerBeat) / sampleRate_
            // This collapses to storing a dimensionless ratio, which we cannot compute here
            // without samplesPerBeat.  Store the raw source-to-target seconds ratio instead,
            // which the decoder uses to drive the stretcher:
            //   local_ratio = targetDeltaSeconds / sourceDeltaSeconds
            // (targetDeltaSeconds comes from the global stretch ratio applied to targetDelta)
            // Approximate: treat globalRatio as 1 beat = 1 second / globalRatio; then
            //   local_ratio = (targetDelta / globalRatio) / sourceDelta
            //   = targetDelta / (sourceDelta * globalRatio)
            return targetDelta / (sourceDelta * globalRatio);
        }
    }
    return globalRatio;
}

// ─────────────────────────────────────────────────────────────────────────────
// fillBlock — render thread
// Pulls numSamples of time-stretched audio into dest (channels 0..nCh-1).
// Never blocks; fills with silence if the output ring is starved.
// ─────────────────────────────────────────────────────────────────────────────
void AudioClipPlayer::fillBlock(juce::AudioBuffer<float>& dest, int numSamples)
{
    if (!loaded_.load(std::memory_order_acquire))
    {
        dest.clear();
        return;
    }

    const int nCh = juce::jmin(numChannels_, dest.getNumChannels());
    int samplesNeeded = numSamples;

    // How many stretched samples are ready in the output ring?
    int available = outputFifo.getNumReady();

    // If not enough, fill the rest with silence.
    int toCopy  = juce::jmin(available, samplesNeeded);
    int silence = samplesNeeded - toCopy;

    if (toCopy > 0)
    {
        int s1, sz1, s2, sz2;
        outputFifo.prepareToRead(toCopy, s1, sz1, s2, sz2);
        for (int ch = 0; ch < nCh; ++ch)
        {
            if (sz1 > 0)
                juce::FloatVectorOperations::copy(dest.getWritePointer(ch),
                                                  outputRing.getReadPointer(ch, s1), sz1);
            if (sz2 > 0)
                juce::FloatVectorOperations::copy(dest.getWritePointer(ch) + sz1,
                                                  outputRing.getReadPointer(ch, s2), sz2);
        }
        outputFifo.finishedRead(toCopy);
    }

    // Silence-fill any starvation gap (write pointer is after toCopy samples).
    if (silence > 0)
        for (int ch = 0; ch < nCh; ++ch)
            juce::FloatVectorOperations::clear(dest.getWritePointer(ch) + toCopy, silence);
}

// ─────────────────────────────────────────────────────────────────────────────
// DecoderThread::run — background decoder loop
// ─────────────────────────────────────────────────────────────────────────────
void AudioClipPlayer::DecoderThread::run()
{
    player.decodeLoop();
}

void AudioClipPlayer::decodeLoop()
{
    if (readerSource == nullptr) return;

    const int kGrainSize = 1024; // samples per decode grain

    while (!decoderThread_.threadShouldExit())
    {
        if (!loaded_.load(std::memory_order_acquire))
        {
            decoderThread_.wait(5);
            continue;
        }

        // ── Handle pending seek ────────────────────────────────────────────
        if (seekPending_.load(std::memory_order_acquire))
        {
            juce::int64 target = seekSample_.load(std::memory_order_acquire);
            seekPending_.store(false, std::memory_order_release);
            readerPosition = target;
            readerSource->setNextReadPosition(target);

            // Reset the stretcher (clears internal delay lines).
            if (stretcher)
                stretcher->reset();

            // Drain output ring so stale data is flushed.
            {
                int s1, sz1, s2, sz2;
                int ready = outputFifo.getNumReady();
                if (ready > 0)
                {
                    outputFifo.prepareToRead(ready, s1, sz1, s2, sz2);
                    outputFifo.finishedRead(sz1 + sz2);
                }
            }
        }

        // ── Back-pressure: wait if output ring is almost full ─────────────
        const int outputFree = outputFifo.getFreeSpace();
        if (outputFree < kGrainSize * 2)
        {
            decoderThread_.wait(2);
            continue;
        }

        // ── Read one grain from the file ───────────────────────────────────
        if (readerPosition >= totalSamples)
        {
            // EOF — pause until a seek resets us.
            decoderThread_.wait(10);
            continue;
        }

        int toRead = juce::jmin(kGrainSize, (int)(totalSamples - readerPosition));
        decodeScratch.clear();
        readerSource->getNextAudioBlock(juce::AudioSourceChannelInfo(&decodeScratch, 0, toRead));
        readerPosition += toRead;

        // ── Feed to stretcher ──────────────────────────────────────────────
        if (!stretcher) continue;

        // Update stretch ratio (may change while playing).
        stretcher->setTimeRatio(stretchRatio.load(std::memory_order_relaxed));

        const float* inPtrs[2] = {
            decodeScratch.getReadPointer(0),
            numChannels_ > 1 ? decodeScratch.getReadPointer(1)
                             : decodeScratch.getReadPointer(0)
        };
        bool isFinal = (readerPosition >= totalSamples);
        stretcher->process(inPtrs, static_cast<size_t>(toRead), isFinal);

        // ── Retrieve output from stretcher into output ring ────────────────
        int available = (int)stretcher->available();
        while (available > 0 && !decoderThread_.threadShouldExit())
        {
            int chunk = juce::jmin(available, kOutputRingCapacity / 4);
            int free  = outputFifo.getFreeSpace();
            if (free < chunk) break; // ring full — try next cycle

            if (retrieveScratch.getNumSamples() < chunk)
                retrieveScratch.setSize(numChannels_, chunk, false, true, false);

            float* outPtrs[2] = {
                retrieveScratch.getWritePointer(0),
                numChannels_ > 1 ? retrieveScratch.getWritePointer(1)
                                 : retrieveScratch.getWritePointer(0)
            };
            stretcher->retrieve(outPtrs, static_cast<size_t>(chunk));

            // Write stretched samples to output ring.
            int s1, sz1, s2, sz2;
            outputFifo.prepareToWrite(chunk, s1, sz1, s2, sz2);
            for (int ch = 0; ch < numChannels_; ++ch)
            {
                if (sz1 > 0)
                    juce::FloatVectorOperations::copy(outputRing.getWritePointer(ch, s1),
                                                      retrieveScratch.getReadPointer(ch), sz1);
                if (sz2 > 0)
                    juce::FloatVectorOperations::copy(outputRing.getWritePointer(ch, s2),
                                                      retrieveScratch.getReadPointer(ch) + sz1, sz2);
            }
            outputFifo.finishedWrite(sz1 + sz2);

            available = (int)stretcher->available();
        }
    }
}
