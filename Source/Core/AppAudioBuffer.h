#pragma once

/**
 * AppAudioBuffer — Application-Level Pre-Render Ring Buffer
 * ============================================================
 * 
 * Purpose:
 *   Decouples the heavy DSP work (pattern sequencing, sampler playback,
 *   plugin processing) from the hardware audio deadline imposed by the
 *   driver callback.
 *
 *   Architecture:
 *
 *   ┌─────────────────────────┐      lock-free FIFO      ┌──────────────────────────────┐
 *   │ Render Thread (non-RT)  │ ──► juce::AbstractFifo ──► │ Audio Thread (RT, minimal)   │
 *   │  - SimplerProcessor     │                           │  - reads pre-rendered frames  │
 *   │  - Pattern sequencing   │                           │  - copies to hardware buffer  │
 *   │  - Plugin processBlock  │                           │  No allocations. No blocking. │
 *   └─────────────────────────┘                           └──────────────────────────────┘
 *
 *   This trades a small amount of added latency (one render block of ~21ms at 48kHz / 1024
 *   samples) for dramatically improved xrun resistance on USB audio interfaces.
 *
 * Thread Safety:
 *   Single-producer (render thread) / single-consumer (audio thread).
 *   Uses juce::AbstractFifo for lock-free coordination. No mutex, ever.
 *
 * Buffer Sizing:
 *   The internal ring buffer holds RING_CAPACITY_BLOCKS hardware blocks worth
 *   of audio. The render thread tries to keep it filled; the audio thread
 *   drains it each callback.
 */

#include <JuceHeader.h>

class AppAudioBuffer
{
public:
    static constexpr int RING_CAPACITY_BLOCKS = 8; // How many full hardware blocks we buffer ahead

    AppAudioBuffer() {}

    /** Called once by prepareToPlay. Allocates the ring buffer. */
    void prepare(int numChannels, int hardwareBlockSize, double sampleRate)
    {
        jassert(numChannels > 0 && hardwareBlockSize > 0);

        numCh          = numChannels;
        blockSize      = hardwareBlockSize;
        fs             = sampleRate;
        const int ringCapacitySamples = blockSize * RING_CAPACITY_BLOCKS;

        ringBuffer.setSize(numCh, ringCapacitySamples);
        ringBuffer.clear();
        fifo.setTotalSize(ringCapacitySamples);
        fifo.reset();
        renderBlockPosition.store(0, std::memory_order_relaxed);
    }

    void releaseResources()
    {
        ringBuffer.setSize(0, 0);
        fifo.reset();
    }

    // ── Producer API (render thread) ─────────────────────────────────────────

    /** Returns true if the FIFO has room for at least one more full hardware block. */
    bool hasRoomToWrite() const noexcept
    {
        return fifo.getFreeSpace() >= blockSize;
    }

    /**
     * Write one rendered hardware block into the ring buffer.
     * @param src  The rendered audio buffer, exactly blockSize samples.
     */
    void writeBlock(const juce::AudioBuffer<float>& src)
    {
        jassert(src.getNumSamples() == blockSize);

        int start1, size1, start2, size2;
        fifo.prepareToWrite(blockSize, start1, size1, start2, size2);

        if (size1 > 0)
            for (int ch = 0; ch < numCh; ++ch)
                ringBuffer.copyFrom(ch, start1, src, juce::jmin(ch, src.getNumChannels()-1), 0, size1);

        if (size2 > 0)
            for (int ch = 0; ch < numCh; ++ch)
                ringBuffer.copyFrom(ch, start2, src, juce::jmin(ch, src.getNumChannels()-1), size1, size2);

        fifo.finishedWrite(size1 + size2);
    }

    // ── Consumer API (audio / hardware callback thread) ──────────────────────

    /** How many samples are ready to be read? */
    int getNumReady() const noexcept { return fifo.getNumReady(); }

    /**
     * Read exactly numSamples from the ring into dest.
     * If insufficient data is available, writes silence (rare → indicates
     * the render thread is falling behind).
     */
    void readBlock(juce::AudioBuffer<float>& dest, int numSamples)
    {
        jassert(numSamples <= dest.getNumSamples());

        if (fifo.getNumReady() < numSamples)
        {
            // Underrun: render thread didn't keep up — fill with silence.
            // This is preferable to outputting garbage or blocking.
            dest.clear(0, numSamples);
            underrunCount.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        int start1, size1, start2, size2;
        fifo.prepareToRead(numSamples, start1, size1, start2, size2);

        if (size1 > 0)
            for (int ch = 0; ch < numCh; ++ch)
                dest.copyFrom(ch, 0, ringBuffer, ch, start1, size1);

        if (size2 > 0)
            for (int ch = 0; ch < numCh; ++ch)
                dest.copyFrom(ch, size1, ringBuffer, ch, start2, size2);

        fifo.finishedRead(size1 + size2);
    }

    /** Underrun counter — poll from UI thread at 30Hz for diagnostics. */
    int getAndResetUnderrunCount()
    {
        return underrunCount.exchange(0, std::memory_order_relaxed);
    }

    int getNumChannels()  const noexcept { return numCh; }
    int getBlockSize()    const noexcept { return blockSize; }
    double getSampleRate() const noexcept { return fs; }

    // Absolute sample position written so far — used by render thread to
    // compute correct transport positions.
    std::atomic<int64_t> renderBlockPosition {0};

private:
    juce::AbstractFifo         fifo        { 1 };
    juce::AudioBuffer<float>   ringBuffer;
    std::atomic<int>           underrunCount {0};

    int    numCh     = 2;
    int    blockSize = 512;
    double fs        = 48000.0;
};
