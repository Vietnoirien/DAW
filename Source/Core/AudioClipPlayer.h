#pragma once

#include <JuceHeader.h>
#include <rubberband/RubberBandStretcher.h>
#include <atomic>
#include <memory>
#include <vector>
#include "ClipData.h"

// ─────────────────────────────────────────────────────────────────────────────
// AudioClipPlayer  (Phase 3.1 — Real-Time Time-Stretching)
//
// Owns a decoded audio file and a RubberBand real-time stretcher.
// The render thread calls fillBlock() to pull time-stretched samples.
// A background decoder thread keeps the input ring buffer filled.
//
// Thread model:
//   Message thread : load() / setWarpMarkers() / reset() (seek)
//   Decoder thread : reads ahead from AudioFormatReader → inputFifo
//   Render thread  : fillBlock() reads outputFifo (never blocks, never allocs)
//
// All public members that may be called from multiple threads are documented
// with which thread(s) may legally call them.
// ─────────────────────────────────────────────────────────────────────────────

class AudioClipPlayer
{
public:
    AudioClipPlayer();
    ~AudioClipPlayer();

    // ── Lifecycle (message thread) ────────────────────────────────────────────

    // Loads an audio file and initialises the stretcher.
    // Call this on the MESSAGE THREAD before the clip becomes active.
    // Calling load() while the clip is playing is safe: the old reader/stretcher
    // is stopped first, then replaced atomically.
    // Returns true on success.
    bool load(const juce::File& file,
              double sampleRate,
              int    maxBlockSize);

    // Release all resources and stop the decoder thread.
    // Call on the message thread (e.g. on track teardown).
    void unload();

    bool isLoaded() const;

    // Duration of the raw audio file (before stretching), in seconds.
    double getLengthSeconds() const;

    // ── Runtime control (message or render thread — atomic) ───────────────────

    // Stretch ratio: > 1.0 = slower (stretch), < 1.0 = faster (squish).
    // Typically set to  projectBPM / clipBpm  when warpEnabled.
    // 1.0 = play at native speed.
    void setStretchRatio(double ratio);
    double getStretchRatio() const;

    // Update warp marker table.  Must be called on the MESSAGE THREAD.
    // Markers must be sorted by targetBeat ascending.
    void setWarpMarkers(const std::vector<WarpMarker>& markers);

    // Seek to a specific beat position within the clip (0 = clip start).
    // Safe to call from the message thread during playback; the decoder
    // will re-synchronise on the next cycle.
    void seek(double clipBeat, double samplesPerBeat);

    // ── Render thread API ─────────────────────────────────────────────────────

    // Pull numSamples of time-stretched audio into dest starting at channel 0.
    // clipSampleOffset: absolute sample offset inside the (stretched) clip — used
    //   only to detect external seeks; normal sequential playback passes -1.
    // If not enough stretched output is available, the remainder is silence
    // (no block, no crash).
    void fillBlock(juce::AudioBuffer<float>& dest, int numSamples);

    // ── Constants ─────────────────────────────────────────────────────────────
    static constexpr int kDecoderRingCapacity = 65536; // ~1.4 s @ 48 kHz
    static constexpr int kOutputRingCapacity  = 32768;

private:
    // ── Decoder thread ────────────────────────────────────────────────────────
    class DecoderThread : public juce::Thread
    {
    public:
        explicit DecoderThread(AudioClipPlayer& owner)
            : juce::Thread("ACP Decoder"), player(owner) {}
        void run() override;
    private:
        AudioClipPlayer& player;
    };

    void decodeLoop();   // called by DecoderThread::run
    void resetStretcher();

    // Compute the stretch ratio for a given beat position within the clip,
    // consulting the warpMarkers table (or falling back to stretchRatio).
    double ratioAtBeat(double clipBeat) const;

    // ── Members ───────────────────────────────────────────────────────────────
    double sampleRate_   = 44100.0;
    int    maxBlockSize_ = 512;

    // Source audio (message thread sets, decoder thread reads after load)
    std::unique_ptr<juce::AudioFormatManager>        formatManager;
    std::unique_ptr<juce::AudioFormatReaderSource>   readerSource;
    juce::int64                                      totalSamples = 0;

    // Stretcher (owned by decoder thread after load; reset on seek)
    std::unique_ptr<RubberBand::RubberBandStretcher> stretcher;
    int                                              stretcherLatency = 0;

    // Ring buffers (lock-free, thread-safe)
    // inputFifo:  decoder → stretcher feeder (raw decoded samples)
    // outputFifo: stretcher output → fillBlock consumer (stretched samples)
    juce::AbstractFifo  inputFifo  { 1 }; // resized in load()
    juce::AudioBuffer<float> inputRing;    // [nCh x kDecoderRingCapacity]

    juce::AbstractFifo  outputFifo { 1 }; // resized in load()
    juce::AudioBuffer<float> outputRing;  // [nCh x kOutputRingCapacity]

    // Reusable scratch buffers allocated in load()
    juce::AudioBuffer<float> decodeScratch;  // one block decoded from file
    juce::AudioBuffer<float> retrieveScratch; // one block retrieved from stretcher

    std::atomic<double> stretchRatio  { 1.0 };
    std::atomic<bool>   loaded_       { false };
    std::atomic<bool>   seekPending_  { false };
    std::atomic<juce::int64> seekSample_ { 0 }; // sample position in raw file
    juce::int64          readerPosition = 0;     // current read head (decoder thread)

    std::vector<WarpMarker> warpMarkers_; // message-thread write, decoder reads (lock protected)
    juce::SpinLock          markerLock_;

    int numChannels_ = 2;

    DecoderThread decoderThread_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioClipPlayer)
};
