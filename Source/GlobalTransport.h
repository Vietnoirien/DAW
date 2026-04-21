#pragma once

#include <JuceHeader.h>
#include <atomic>
#include <cmath>

class GlobalTransport {
public:
    GlobalTransport() = default;

    void setSampleRate(double newSampleRate) {
        sampleRate = newSampleRate;
        updateSamplesPerBeat();
    }

    void setBpm(double newBpm) {
        bpm.store(newBpm, std::memory_order_relaxed);
        updateSamplesPerBeat();
    }

    void play() { isPlaying.store(true, std::memory_order_release); }
    
    void stop() { 
        isPlaying.store(false, std::memory_order_release);
        playheadPosition.store(0, std::memory_order_release);
    }

    void advanceBy(int numSamples) {
        if (isPlaying.load(std::memory_order_acquire)) {
            playheadPosition.fetch_add(numSamples, std::memory_order_acq_rel);
        }
    }

    bool getIsPlaying() const { return isPlaying.load(std::memory_order_acquire); }
    int64_t getPlayheadPosition() const { return playheadPosition.load(std::memory_order_acquire); }
    double getSamplesPerBeat() const { return samplesPerBeat.load(std::memory_order_acquire); }
    double getBpm() const { return bpm.load(std::memory_order_acquire); }

    // Calculates the absolute sample offset for the start of the next exact bar.
    double getNextBarPosition(int beatsPerBar = 4) const {
        double spb = samplesPerBeat.load(std::memory_order_acquire);
        if (spb <= 0.0) return 0;
        
        double samplesPerBar = spb * beatsPerBar;
        double pos = static_cast<double>(playheadPosition.load(std::memory_order_acquire));
        
        // Find next exact bar boundary
        double nextBarSamples = std::ceil(pos / samplesPerBar) * samplesPerBar;
        
        // If we are extremely close to the boundary, give the next one to avoid scheduling in the past
        if (nextBarSamples - pos < 1.0) {
            nextBarSamples += samplesPerBar;
        }
        
        return nextBarSamples;
    }

private:
    void updateSamplesPerBeat() {
        if (sampleRate > 0.0 && bpm.load(std::memory_order_relaxed) > 0.0) {
            double spb = sampleRate * 60.0 / bpm.load(std::memory_order_relaxed);
            samplesPerBeat.store(spb, std::memory_order_release);
        }
    }

    std::atomic<bool> isPlaying {false};
    std::atomic<int64_t> playheadPosition {0}; // Absolute sample position since play start
    std::atomic<double> bpm {120.0};
    std::atomic<double> samplesPerBeat {0.0};
    double sampleRate {44100.0};
};
