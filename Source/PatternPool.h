#pragma once

#include <JuceHeader.h>
#include "LockFreeQueue.h"
#include <memory>
#include <vector>

template <typename T, int Capacity = 256>
class PatternPool {
public:
    PatternPool() {
        for (int i = 0; i < Capacity; ++i) {
            pool.push_back(std::make_unique<T>());
            freeIndicies.push(i);
        }
    }

    T* rentPattern() {
        if (auto index = freeIndicies.pop()) {
            return pool[*index].get();
        }
        // In a real production scenario we might dynamically allocate slightly here,
        // but strict real-time patterns dictate static pooling. Pool exhaustion is an error.
        jassertfalse; 
        return nullptr;
    }

    void returnPattern(T* pattern) {
        if (pattern == nullptr) return;
        
        for (int i = 0; i < Capacity; ++i) {
            if (pool[i].get() == pattern) {
                pattern->clear(); 
                freeIndicies.push(i);
                return;
            }
        }
        jassertfalse; // Tried to return a pattern that doesn't belong to this pool
    }

private:
    std::vector<std::unique_ptr<T>> pool;
    LockFreeQueue<int, Capacity> freeIndicies;
};
