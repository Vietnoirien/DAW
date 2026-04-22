#include "DrumRackProcessor.h"
#include "DrumRackComponent.h"

std::unique_ptr<juce::Component> DrumRackProcessor::createEditor() {
    return std::make_unique<DrumRackComponent>(this);
}
