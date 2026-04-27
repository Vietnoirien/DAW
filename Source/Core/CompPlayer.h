#pragma once

#include <JuceHeader.h>
#include <atomic>
#include <array>
#include <memory>
#include <vector>
#include "ClipData.h"
#include "AudioClipPlayer.h"
#include "LockFreeQueue.h"

// ─────────────────────────────────────────────────────────────────────────────
// CompPlayer  (Phase 3.2 — Non-Destructive Comping)
//
// Owns up to Take::kMaxTakesPerClip (8) AudioClipPlayer instances, one per
// recorded take.  The render thread calls fillBlock() to pull the comped
// output.  At any sample boundary, the active take is determined by the
// sorted CompRegion table; equal-power crossfades are applied at boundaries
// using a pre-allocated fade table — no heap allocation ever occurs inside
// fillBlock().
//
// Thread model (matches AudioClipPlayer conventions):
//   Message thread : setTakes() / setCompRegions() / unloadAll()
//   Decoder threads: one per AudioClipPlayer (N ≤ 8)
//   Render thread  : fillBlock()  (never blocks, never allocs)
// ─────────────────────────────────────────────────────────────────────────────

class CompPlayer
{
public:
    CompPlayer();
    ~CompPlayer();

    // ── Lifecycle (message thread) ────────────────────────────────────────────

    // Load all takes.  Creates / reloads AudioClipPlayer instances for each take.
    // Must be called on the MESSAGE THREAD.  The players start loading on
    // background threads; poll allPlayersLoaded() before calling fillBlock().
    void setTakes(const std::vector<Take>& takes,
                  double sampleRate,
                  int    maxBlockSize);

    // Atomically replace the comp region table.
    // Old pointer is pushed into regionGarbage_ for GC by the render thread.
    // Must be called on the MESSAGE THREAD.
    void setCompRegions(const std::vector<CompRegion>& regions);

    // Release all resources and stop all decoder threads.
    void unloadAll();

    // Returns true once every take player reports isLoaded().
    bool allPlayersLoaded() const;

    // Number of takes currently loaded (≤ kMaxTakesPerClip).
    int getNumTakes() const;

    // ── Render thread API ─────────────────────────────────────────────────────

    // Drain the region garbage queue (call at the top of each render block).
    void drainRegionGarbage();

    // Pull numSamples of comped audio into dest.
    // clipBeatOffset: the beat position inside the clip at the start of this block
    //   (0.0 = clip start), used to look up the active CompRegion.
    // samplesPerBeat: current project tempo mapping (for beat→sample conversion).
    // If no comp region covers the current position, silence is written.
    void fillBlock(juce::AudioBuffer<float>& dest,
                   int    numSamples,
                   double clipBeatOffset,
                   double samplesPerBeat);

    // ── Constants ─────────────────────────────────────────────────────────────
    // Fade table length = the maximum crossfade half-length in samples.
    // ~42 ms @ 48 kHz — longer than the default 10 ms to allow user extension.
    static constexpr int kMaxFadeLen = 2048;

private:
    // Pre-computed equal-power fade-out curve:
    //   fadeTable_[i] = cos( π/2 * i / kMaxFadeLen )
    // Fade-in = sin = cos of the complement; computed as sqrt(1 - cos²) or
    // simply accessed as fadeTable_[kMaxFadeLen - i].
    std::array<float, kMaxFadeLen> fadeTable_;

    double sampleRate_   = 44100.0;
    int    maxBlockSize_ = 512;
    int    numChannels_  = 2;

    // One AudioClipPlayer per take (index matches Take::takeIndex).
    // Size ≤ Take::kMaxTakesPerClip.
    std::vector<std::unique_ptr<AudioClipPlayer>> players_;

    // Double-buffered comp region list.
    // Message thread writes via setCompRegions(); render thread reads via load(acquire).
    std::atomic<std::vector<CompRegion>*> compRegions_ { nullptr };
    LockFreeQueue<std::vector<CompRegion>*, 8> regionGarbage_;

    // Pre-allocated crossfade scratch buffers (allocated once in setTakes()).
    // fadeOutScratch_: audio from the outgoing take during a crossfade zone.
    // fadeInScratch_:  audio from the incoming take during a crossfade zone.
    juce::AudioBuffer<float> fadeOutScratch_;
    juce::AudioBuffer<float> fadeInScratch_;

    // Find the CompRegion that owns clipBeat, or nullptr if none.
    // Called only from the render thread — reads compRegions_ atomically.
    const CompRegion* regionAtBeat(const std::vector<CompRegion>* regions,
                                   double clipBeat) const noexcept;

    // Compute cos/sin gains from the fade table for sample i of a crossfade
    // of total length fadeLenSamples.
    void fadeGainsAt(int i, int fadeLenSamples,
                     float& gainOut, float& gainIn) const noexcept;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CompPlayer)
};
