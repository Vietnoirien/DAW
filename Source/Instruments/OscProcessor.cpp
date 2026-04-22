#include "OscProcessor.h"
#include "OscComponent.h"

std::unique_ptr<juce::Component> OscProcessor::createEditor() {
    return std::make_unique<OscComponent>(this);
}
