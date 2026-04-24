#include <JuceHeader.h>
int main() {
    juce::SpinLock lock;
    const juce::SpinLock::ScopedLockType sl(lock);
    return 0;
}
