#pragma once
#include <JuceHeader.h>

class SimplerComponent : public juce::Component, private juce::ChangeListener {
public:
    SimplerComponent() 
        : thumbnailCache(5), 
          thumbnail(1024, formatManager, thumbnailCache)
    {
        setOpaque(true);
        formatManager.registerBasicFormats();
        
        thumbnail.addChangeListener(this);
    }

    ~SimplerComponent() override {
        thumbnail.removeChangeListener(this);
    }

    void loadFile(const juce::File& file) {
        loadedFile = file;
        thumbnail.setSource(new juce::FileInputSource(file));
        repaint();
    }

    // Pure display update — called by DeviceView when the user switches track selection.
    // Does NOT trigger onFileDropped. Only refreshes the waveform thumbnail.
    void showFile(const juce::File& file) {
        if (file == loadedFile) return;  // already displayed, nothing to do
        loadedFile = file;
        if (file.existsAsFile())
            thumbnail.setSource(new juce::FileInputSource(file));
        else
            thumbnail.clear();
        repaint();
    }

    void paint(juce::Graphics& g) override {
        g.fillAll(juce::Colours::darkgrey.darker());
        
        // Draw the waveform view background
        auto area = getLocalBounds().reduced(10);
        auto waveformArea = area.removeFromTop(area.getHeight() * 0.7f);
        
        g.setColour(juce::Colours::black);
        g.fillRoundedRectangle(waveformArea.toFloat(), 5.0f);
        g.setColour(juce::Colours::grey);
        g.drawRoundedRectangle(waveformArea.toFloat(), 5.0f, 2.0f);
        
        // Draw waveform
        if (thumbnail.getTotalLength() > 0.0) {
            g.setColour(juce::Colours::lightgreen);
            thumbnail.drawChannels(g, waveformArea.reduced(5), 0.0, thumbnail.getTotalLength(), 1.0f);
        } else if (loadedFile.exists()) {
            g.setColour(juce::Colours::white);
            g.drawText("Loading waveform...", waveformArea, juce::Justification::centred);
        }

        // Draw Parametric UI area
        auto paramArea = area;
        g.setColour(juce::Colours::white);
        g.drawText(loadedFile.getFileName(), paramArea.removeFromTop(20), juce::Justification::centred);

        // Mock knobs via simple loop
        int numKnobs = 4;
        float knobWidth = 40.0f;
        float spacing = (paramArea.getWidth() - (numKnobs * knobWidth)) / (numKnobs + 1.0f);
        
        for (int i = 0; i < numKnobs; ++i) {
            float x = paramArea.getX() + spacing + i * (knobWidth + spacing);
            float y = paramArea.getY() + (paramArea.getHeight() - knobWidth) / 2.0f;
            
            g.setColour(juce::Colours::grey);
            g.fillEllipse(x, y, knobWidth, knobWidth);
            g.setColour(juce::Colours::white);
            g.drawEllipse(x, y, knobWidth, knobWidth, 2.0f);
            
            // Draw a pseudo-indicator
            g.drawLine(x + knobWidth / 2.0f, y + knobWidth / 2.0f, x + knobWidth / 2.0f, y + 5.0f, 2.0f);
        }
    }

    void resized() override {
        // Handled in paint for simplicity of drawing mockup
    }

private:
    void changeListenerCallback(juce::ChangeBroadcaster* source) override {
        if (source == &thumbnail) {
            repaint();
        }
    }

    juce::File loadedFile;
    juce::AudioFormatManager formatManager;
    juce::AudioThumbnailCache thumbnailCache;
    juce::AudioThumbnail thumbnail;
};
