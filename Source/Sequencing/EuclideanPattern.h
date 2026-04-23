#pragma once

#include <JuceHeader.h>
#include "Pattern.h"
#include <vector>

class EuclideanPattern : public Pattern {
public:
    EuclideanPattern() {
        hitMap.reserve(64); // Support up to 64 steps without reallocation
    }

    // Bjorklund's algorithm equivalent using Bresenham line approach
    // k = pulses (hits), n = steps 
    void generate(int k, int n) {
        hitMap.clear();
        
        // Guard against improper bounds
        if (n <= 0) return;
        if (k > n) k = n;
        
        hitMap.resize(n, 0);

        if (k == 0) return;

        for (int i = 0; i < n; ++i) {
            hitMap[i] = ((i * k) % n < k) ? 1 : 0;
        }
    }

    void setHitMap(const std::vector<uint8_t>& newMap) {
        hitMap = newMap;
    }

    const std::vector<uint8_t>& getHitMap() const { return hitMap; }

    void getEventsForBuffer(juce::MidiBuffer& output, int64_t blockStartSample, int numSamples, const GlobalTransport& transport, double patternStartSample) const override {
        if (hitMap.empty()) return;

        double spb = transport.getSamplesPerBeat();
        if (spb <= 0.0) return;

        double stepDurationSamples = spb / 4.0;
        int n = static_cast<int>(hitMap.size());
        double patternLengthSamples = stepDurationSamples * n;

        // Ensure phase is strictly positive relative to the start sample
        if (static_cast<double>(blockStartSample + numSamples) <= patternStartSample) return;

        double blockStart = static_cast<double>(blockStartSample) - patternStartSample;
        double blockEnd = blockStart + numSamples;

        // If blockStart is negative, clamp it to 0 so we don't schedule events before the trigger time
        if (blockStart < 0.0) {
            blockStart = 0.0;
        }

        for (int i = 0; i < n; ++i) {
            if (hitMap[i] == 0) continue;

            double hitStartOffset = i * stepDurationSamples;
            double hitEndOffset = hitStartOffset + (stepDurationSamples / 2.0);

            // Find all Note On events falling in [blockStart, blockEnd)
            long kStartOn = static_cast<long>(std::ceil((blockStart - hitStartOffset) / patternLengthSamples)) - 1;
            long kEndOn = static_cast<long>(std::ceil((blockEnd - hitStartOffset) / patternLengthSamples)) + 1;

            for (long k = kStartOn; k <= kEndOn; ++k) {
                double exactStart = k * patternLengthSamples + hitStartOffset;
                int64_t globalSample = static_cast<int64_t>(std::round(exactStart + patternStartSample));
                
                if (globalSample >= blockStartSample && globalSample < blockStartSample + numSamples) {
                    int bufferOffset = static_cast<int>(globalSample - blockStartSample);
                    output.addEvent(juce::MidiMessage::noteOn(1, 60, 1.0f), bufferOffset);
                }
            }

            // Find all Note Off events falling in [blockStart, blockEnd)
            long kStartOff = static_cast<long>(std::ceil((blockStart - hitEndOffset) / patternLengthSamples)) - 1;
            long kEndOff = static_cast<long>(std::ceil((blockEnd - hitEndOffset) / patternLengthSamples)) + 1;

            for (long k = kStartOff; k <= kEndOff; ++k) {
                double exactEnd = k * patternLengthSamples + hitEndOffset;
                int64_t globalSample = static_cast<int64_t>(std::round(exactEnd + patternStartSample));
                
                if (globalSample >= blockStartSample && globalSample < blockStartSample + numSamples) {
                    int bufferOffset = static_cast<int>(globalSample - blockStartSample);
                    output.addEvent(juce::MidiMessage::noteOff(1, 60, 0.0f), bufferOffset);
                }
            }
        }
    }

    void clear() override {
        Pattern::clear();
        hitMap.clear();
    }

    double getLengthBeats() const override {
        return static_cast<double>(hitMap.size()) / 4.0;
    }

private:
    std::vector<uint8_t> hitMap; // uint8_t is thread safer and faster than vector<bool>
};
