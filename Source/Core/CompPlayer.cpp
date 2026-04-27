#include "CompPlayer.h"
#include <cmath>

// ─────────────────────────────────────────────────────────────────────────────
// CompPlayer
// ─────────────────────────────────────────────────────────────────────────────

CompPlayer::CompPlayer()
{
    // Pre-compute the equal-power fade table: fadeTable_[i] = cos(π/2 · i/N)
    // This gives the fade-out gain.  Fade-in gain = fadeTable_[N - i].
    for (int i = 0; i < kMaxFadeLen; ++i)
        fadeTable_[i] = std::cos(static_cast<float>(i) / kMaxFadeLen
                                 * juce::MathConstants<float>::halfPi);
}

CompPlayer::~CompPlayer()
{
    unloadAll();
}

// ─────────────────────────────────────────────────────────────────────────────
// setTakes  — message thread
// ─────────────────────────────────────────────────────────────────────────────
void CompPlayer::setTakes(const std::vector<Take>& takes,
                          double sampleRate,
                          int    maxBlockSize)
{
    sampleRate_   = sampleRate;
    maxBlockSize_ = maxBlockSize;
    numChannels_  = 2; // stereo throughout

    // Clamp to the hard limit
    const int n = static_cast<int>(
        std::min(takes.size(), static_cast<size_t>(Take::kMaxTakesPerClip)));

    // Resize the player pool to match the new take count.
    // Only reallocate if count has grown — never on the render thread.
    while (static_cast<int>(players_.size()) < n)
        players_.push_back(std::make_unique<AudioClipPlayer>());

    // Unload any players beyond the new count.
    for (int i = n; i < static_cast<int>(players_.size()); ++i)
        players_[i]->unload();
    players_.resize(n);

    // Load each take (background decode threads are started inside load()).
    for (int i = 0; i < n; ++i)
    {
        juce::File f(takes[i].audioFilePath);
        if (f.existsAsFile())
            players_[i]->load(f, sampleRate, maxBlockSize);
    }

    // Pre-allocate crossfade scratch buffers (render thread reads these later).
    fadeOutScratch_.setSize(numChannels_, kMaxFadeLen, false, true, false);
    fadeInScratch_ .setSize(numChannels_, kMaxFadeLen, false, true, false);
}

// ─────────────────────────────────────────────────────────────────────────────
// setCompRegions  — message thread
// ─────────────────────────────────────────────────────────────────────────────
void CompPlayer::setCompRegions(const std::vector<CompRegion>& regions)
{
    auto* newRegions = new std::vector<CompRegion>(regions);
    // Ensure regions are sorted by startBeat
    std::sort(newRegions->begin(), newRegions->end(),
        [](const CompRegion& a, const CompRegion& b) { return a.startBeat < b.startBeat; });

    auto* old = compRegions_.exchange(newRegions, std::memory_order_acq_rel);
    if (old != nullptr)
        regionGarbage_.push(old);
}

// ─────────────────────────────────────────────────────────────────────────────
// unloadAll  — message thread
// ─────────────────────────────────────────────────────────────────────────────
void CompPlayer::unloadAll()
{
    for (auto& p : players_)
        if (p) p->unload();
    players_.clear();

    auto* old = compRegions_.exchange(nullptr, std::memory_order_acq_rel);
    delete old;
    while (auto optPtr = regionGarbage_.pop())
        delete *optPtr;
}

// ─────────────────────────────────────────────────────────────────────────────
bool CompPlayer::allPlayersLoaded() const
{
    for (const auto& p : players_)
        if (p && !p->isLoaded()) return false;
    return !players_.empty();
}

int CompPlayer::getNumTakes() const
{
    return static_cast<int>(players_.size());
}

// ─────────────────────────────────────────────────────────────────────────────
// drainRegionGarbage  — render thread (top of block)
// ─────────────────────────────────────────────────────────────────────────────
void CompPlayer::drainRegionGarbage()
{
    while (auto optPtr = regionGarbage_.pop())
        delete *optPtr;
}

// ─────────────────────────────────────────────────────────────────────────────
// regionAtBeat  — render thread helper (no alloc, no lock)
// ─────────────────────────────────────────────────────────────────────────────
const CompRegion* CompPlayer::regionAtBeat(const std::vector<CompRegion>* regions,
                                            double clipBeat) const noexcept
{
    if (regions == nullptr || regions->empty()) return nullptr;
    // Linear scan — region count is tiny (≤ 64 typical)
    for (const auto& r : *regions)
        if (clipBeat >= r.startBeat && clipBeat < r.endBeat)
            return &r;
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// fadeGainsAt  — render thread helper (reads pre-computed table only)
// ─────────────────────────────────────────────────────────────────────────────
void CompPlayer::fadeGainsAt(int i, int fadeLenSamples,
                              float& gainOut, float& gainIn) const noexcept
{
    const int clamped = juce::jlimit(0, kMaxFadeLen - 1, fadeLenSamples);
    const int idx     = juce::jlimit(0, kMaxFadeLen - 1,
                                     (i * kMaxFadeLen) / std::max(1, clamped));
    gainOut = fadeTable_[idx];
    gainIn  = fadeTable_[kMaxFadeLen - 1 - idx]; // sin(x) = cos(π/2 - x)
}

// ─────────────────────────────────────────────────────────────────────────────
// fillBlock  — render thread (no alloc, no lock, no block)
// ─────────────────────────────────────────────────────────────────────────────
void CompPlayer::fillBlock(juce::AudioBuffer<float>& dest,
                           int    numSamples,
                           double clipBeatOffset,
                           double samplesPerBeat)
{
    dest.clear();
    if (players_.empty() || samplesPerBeat <= 0.0) return;

    const auto* regions = compRegions_.load(std::memory_order_acquire);
    const int   nCh     = dest.getNumChannels();

    // Determine active region at the start of this block.
    const CompRegion* region = regionAtBeat(regions, clipBeatOffset);

    // If no comp regions exist but we have at least one take, fall back to take 0.
    int activeTakeIdx = 0;
    if (region != nullptr)
        activeTakeIdx = juce::jlimit(0, static_cast<int>(players_.size()) - 1,
                                     region->takeIndex);

    auto* activePlayer = (activeTakeIdx < static_cast<int>(players_.size()))
                             ? players_[activeTakeIdx].get()
                             : nullptr;
    if (activePlayer == nullptr || !activePlayer->isLoaded()) return;

    // ── Detect whether a region boundary falls inside this block ──────────────
    // Look for the NEXT region boundary after clipBeatOffset.
    const CompRegion* nextRegion = nullptr;
    if (regions != nullptr)
    {
        for (const auto& r : *regions)
        {
            double rStartBeat = r.startBeat;
            if (rStartBeat > clipBeatOffset)
            {
                double rStartSample = (rStartBeat - clipBeatOffset) * samplesPerBeat;
                if (rStartSample < numSamples)
                {
                    nextRegion = &r;
                    break;
                }
            }
        }
    }

    if (nextRegion == nullptr)
    {
        // ── Simple case: no boundary in this block — pull straight from active player.
        activePlayer->fillBlock(dest, numSamples);
        return;
    }

    // ── Crossfade case ────────────────────────────────────────────────────────
    // Boundary is at sample 'boundSample' within this block.
    const int boundSample = juce::jlimit(
        0, numSamples - 1,
        static_cast<int>((nextRegion->startBeat - clipBeatOffset) * samplesPerBeat));

    const int incomingTakeIdx = juce::jlimit(
        0, static_cast<int>(players_.size()) - 1, nextRegion->takeIndex);

    AudioClipPlayer* incomingPlayer = (incomingTakeIdx < static_cast<int>(players_.size()))
                                          ? players_[incomingTakeIdx].get()
                                          : nullptr;

    // Fade parameters (capped to kMaxFadeLen)
    const int fadeLen = juce::jlimit(1, kMaxFadeLen,
        std::max(nextRegion->fadeInSamples, nextRegion->fadeOutSamples));

    // ── Part A: before the boundary — outgoing take, no blend ─────────────────
    if (boundSample > 0)
    {
        // Fill only the first 'boundSample' samples from the active player.
        // We borrow fadeOutScratch_ as a temp target, then copy out.
        fadeOutScratch_.clear();
        // Temporarily resize our view (no realloc — scratch is pre-sized to kMaxFadeLen)
        // We'll use a temporary local buffer.  Use a thread_local scratch here
        // to avoid allocation.  Actually, dest has room; fill it directly up to bound.
        // Use a two-step approach: fillBlock writes numSamples; we cap by overlay.
        static thread_local juce::AudioBuffer<float> partA;
        if (partA.getNumChannels() < nCh || partA.getNumSamples() < numSamples)
            partA.setSize(nCh, numSamples, false, true, false);
        partA.clear();
        activePlayer->fillBlock(partA, boundSample);
        for (int ch = 0; ch < nCh; ++ch)
            juce::FloatVectorOperations::copy(dest.getWritePointer(ch),
                                              partA.getReadPointer(ch),
                                              boundSample);
    }

    // ── Part B: crossfade zone around the boundary ─────────────────────────
    // Pull kFadeHalf samples from each player into scratch buffers.
    const int fadeHalf = std::min(fadeLen, std::min(boundSample, numSamples - boundSample));
    if (fadeHalf > 0 && incomingPlayer != nullptr && incomingPlayer->isLoaded())
    {
        // Pull fade-out from active player (continuation after part A)
        fadeOutScratch_.clear();
        activePlayer->fillBlock(fadeOutScratch_, fadeHalf);

        // Pull fade-in from incoming player (it should already be at the right position
        // if it was previously the active take; otherwise it starts from its beginning).
        fadeInScratch_.clear();
        incomingPlayer->fillBlock(fadeInScratch_, fadeHalf);

        // Blend into dest at boundSample with equal-power gains
        for (int i = 0; i < fadeHalf; ++i)
        {
            float gainOut, gainIn;
            fadeGainsAt(i, fadeHalf, gainOut, gainIn);
            const int destIdx = boundSample + i;
            if (destIdx >= numSamples) break;
            for (int ch = 0; ch < nCh; ++ch)
            {
                const float out = fadeOutScratch_.getReadPointer(ch)[i];
                const float in  = fadeInScratch_ .getReadPointer(ch)[i];
                dest.getWritePointer(ch)[destIdx] = out * gainOut + in * gainIn;
            }
        }
    }

    // ── Part C: after the crossfade — incoming take runs straight ─────────────
    const int afterFadeStart = boundSample + fadeHalf;
    const int afterFadeLen   = numSamples - afterFadeStart;
    if (afterFadeLen > 0 && incomingPlayer != nullptr && incomingPlayer->isLoaded())
    {
        static thread_local juce::AudioBuffer<float> partC;
        if (partC.getNumChannels() < nCh || partC.getNumSamples() < afterFadeLen)
            partC.setSize(nCh, afterFadeLen, false, true, false);
        partC.clear();
        incomingPlayer->fillBlock(partC, afterFadeLen);
        for (int ch = 0; ch < nCh; ++ch)
            juce::FloatVectorOperations::copy(dest.getWritePointer(ch) + afterFadeStart,
                                              partC.getReadPointer(ch),
                                              afterFadeLen);
    }
}
