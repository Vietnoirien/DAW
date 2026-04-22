#include "SimplerProcessor.h"
#include "SimplerComponent.h"

std::unique_ptr<juce::Component> SimplerProcessor::createEditor() {
    return std::make_unique<SimplerComponent>(this);
}
