#pragma once

#include <JuceHeader.h>
#include <array>
#include <optional>

template <typename T, int Capacity = 128>
class LockFreeQueue {
public:
    LockFreeQueue() : fifo(Capacity) {}

    bool push(const T& item) {
        auto writeHandle = fifo.write(1);
        if (writeHandle.blockSize1 > 0) {
            buffer[(size_t)writeHandle.startIndex1] = item;
            return true;
        }
        return false;
    }

    std::optional<T> pop() {
        auto readHandle = fifo.read(1);
        if (readHandle.blockSize1 > 0) {
            return buffer[(size_t)readHandle.startIndex1];
        }
        return std::nullopt;
    }

private:
    juce::AbstractFifo fifo;
    std::array<T, Capacity> buffer;
};
