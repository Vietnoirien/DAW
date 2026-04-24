#pragma once
#include <JuceHeader.h>

// ============================================================
// LiBeLookAndFeel
// Shared rotary slider renderer for all LiBeDAW instruments.
// Reads from slider properties:
//   "accentColour"      (juce::int64 ARGB)  — per-instrument fill arc colour
//   "isAutomated"       (bool)               — draws orange outer ring
//   "automationMinNorm" (float 0..1)         — automation range arc start
//   "automationMaxNorm" (float 0..1)         — automation range arc end
// ============================================================
class LiBeLookAndFeel : public juce::LookAndFeel_V4 {
public:
    explicit LiBeLookAndFeel(juce::Colour accent = juce::Colour(0xffFFAA00)) : defaultAccent(accent) {
        setColour(juce::Slider::rotarySliderFillColourId,    accent);
        setColour(juce::Slider::rotarySliderOutlineColourId, juce::Colour(0xff1A1A2A));
        setColour(juce::Slider::thumbColourId,               accent.brighter(0.4f));
        setColour(juce::ComboBox::backgroundColourId,        juce::Colour(0xff111118));
        setColour(juce::ComboBox::textColourId,              accent);
        setColour(juce::ComboBox::outlineColourId,           juce::Colour(0xff222230));
        setColour(juce::ComboBox::arrowColourId,             accent);
        setColour(juce::ToggleButton::textColourId,          accent);
        setColour(juce::ToggleButton::tickColourId,          accent);
        setColour(juce::ToggleButton::tickDisabledColourId,  accent.withAlpha(0.4f));
        setColour(juce::Label::textColourId,                 accent);
        setColour(juce::PopupMenu::backgroundColourId,       juce::Colour(0xff111118));
        setColour(juce::PopupMenu::textColourId,             juce::Colour(0xffCCCCCC));
        setColour(juce::PopupMenu::highlightedBackgroundColourId, accent.withAlpha(0.3f));
    }

    void drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height,
                          float sliderPos, float rotaryStartAngle, float rotaryEndAngle,
                          juce::Slider& slider) override
    {
        const float cx     = x + width  * 0.5f;
        const float cy     = y + height * 0.5f;
        const float radius = (float)juce::jmin(width / 2, height / 2) - 4.0f;

        if (radius < 2.0f) return;

        const float angle = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);

        // Read per-slider accent colour (falls back to LAF default)
        juce::Colour accent = defaultAccent;
        if (slider.getProperties().contains("accentColour")) {
            accent = juce::Colour((juce::uint32)(juce::int64)slider.getProperties()["accentColour"]);
        }

        const bool isAutomated = slider.getProperties().contains("isAutomated")
                                 && (bool)slider.getProperties()["isAutomated"];

        // ── 1. Background track arc ──────────────────────────────────────
        juce::Path trackArc;
        trackArc.addCentredArc(cx, cy, radius, radius, 0.0f, rotaryStartAngle, rotaryEndAngle, true);
        g.setColour(juce::Colour(0xff1A1A2A));
        g.strokePath(trackArc, juce::PathStrokeType(3.0f));

        // ── 2. Automation range arc (drawn between track arc and knob body) ──
        if (isAutomated
            && slider.getProperties().contains("automationMinNorm")
            && slider.getProperties().contains("automationMaxNorm"))
        {
            float minNorm = (float)slider.getProperties()["automationMinNorm"];
            float maxNorm = (float)slider.getProperties()["automationMaxNorm"];
            float aMin = rotaryStartAngle + minNorm * (rotaryEndAngle - rotaryStartAngle);
            float aMax = rotaryStartAngle + maxNorm * (rotaryEndAngle - rotaryStartAngle);
            float rng  = radius + 5.0f;

            if (aMax > aMin + 0.001f) {
                // Draw a swept arc showing the automation range
                juce::Path rangeArc;
                rangeArc.addCentredArc(cx, cy, rng, rng, 0.0f, aMin, aMax, true);
                g.setColour(juce::Colour(0xffFF8800).withAlpha(0.85f));
                g.strokePath(rangeArc, juce::PathStrokeType(3.5f, juce::PathStrokeType::curved,
                                                             juce::PathStrokeType::rounded));
            } else {
                // Flat lane or single-point: draw a bright tick at the automation value
                float aMid = (aMin + aMax) * 0.5f;
                float tx   = cx + rng * std::sin(aMid);
                float ty   = cy - rng * std::cos(aMid);
                g.setColour(juce::Colour(0xffFF8800));
                g.fillEllipse(tx - 3.0f, ty - 3.0f, 6.0f, 6.0f);
            }
        }

        // ── 3. Filled value arc ──────────────────────────────────────────
        juce::Path fillArc;
        fillArc.addCentredArc(cx, cy, radius, radius, 0.0f, rotaryStartAngle, angle, true);
        g.setColour(accent);
        g.strokePath(fillArc, juce::PathStrokeType(3.0f));

        // ── 4. Knob body ─────────────────────────────────────────────────
        const float bodyR = radius * 0.58f;
        g.setColour(juce::Colour(0xff0D0D14));
        g.fillEllipse(cx - bodyR, cy - bodyR, bodyR * 2.0f, bodyR * 2.0f);

        // ── 5. Pointer ───────────────────────────────────────────────────
        juce::Path pointer;
        pointer.addRectangle(-1.5f, -(bodyR * 0.85f), 3.0f, bodyR * 0.85f);
        pointer.applyTransform(juce::AffineTransform::rotation(angle).translated(cx, cy));
        g.setColour(accent.brighter(0.3f));
        g.fillPath(pointer);

        // ── 6. Automation ring (outer) ───────────────────────────────────
        if (isAutomated) {
            g.setColour(juce::Colour(0xffFF6600));
            g.drawEllipse(cx - bodyR - 2.5f, cy - bodyR - 2.5f,
                          (bodyR + 2.5f) * 2.0f, (bodyR + 2.5f) * 2.0f, 2.0f);
        }
    }

    void drawToggleButton(juce::Graphics& g, juce::ToggleButton& btn,
                          bool shouldDrawButtonAsHighlighted,
                          bool shouldDrawButtonAsDown) override
    {
        bool on = btn.getToggleState();
        auto b  = btn.getLocalBounds().toFloat().reduced(1.0f);

        g.setColour(on ? defaultAccent.withAlpha(0.25f) : juce::Colour(0xff111118));
        g.fillRoundedRectangle(b, 3.0f);
        g.setColour(on ? defaultAccent : defaultAccent.withAlpha(0.35f));
        g.drawRoundedRectangle(b, 3.0f, 1.0f);
        g.setColour(on ? defaultAccent : juce::Colour(0xff555566));
        g.setFont(juce::FontOptions(10.0f, juce::Font::bold));
        g.drawText(btn.getButtonText(), btn.getLocalBounds(), juce::Justification::centred);
    }

    juce::Colour defaultAccent;
};

// ============================================================
// LiBeKnob — shared knob+label widget
//
// • Draws label below the slider
// • Sets "accentColour" property on the slider
// • Exposes slider directly so caller can connect onValueChange
// • Supports automation live-tracking via startTrackingParam()
// ============================================================
class LiBeKnob : public juce::Component, private juce::Timer {
public:
    juce::Slider slider;

    LiBeKnob() = default;

    void setup(const juce::String& lbl, double val, double minV, double maxV,
               double defaultV, const juce::String& paramId, juce::Colour accent)
    {
        labelText = lbl;
        accentCol = accent;
        addAndMakeVisible(slider);
        slider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        slider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        slider.setRange(minV, maxV);
        slider.setValue(val, juce::dontSendNotification);
        slider.setDoubleClickReturnValue(true, defaultV < 0.0 ? val : defaultV);
        slider.getProperties().set("accentColour", (juce::int64)accent.getARGB());
        if (paramId.isNotEmpty())
            slider.getProperties().set("parameterId", paramId);
    }

    // Called by DeviceView::updateAutomationIndicators
    void setAutomated(bool automated, float minNorm = 0.f, float maxNorm = 1.f) {
        slider.getProperties().set("isAutomated",       automated);
        slider.getProperties().set("automationMinNorm", minNorm);
        slider.getProperties().set("automationMaxNorm", maxNorm);
        // Always repaint so range updates are visible immediately
        slider.repaint();
    }

    // Starts the 30Hz live-tracking timer that reads from paramPtr
    void startTrackingParam(std::atomic<float>* ptr, float minVal, float maxVal) {
        trackPtr  = ptr;
        trackMin  = minVal;
        trackMax  = maxVal;
        startTimerHz(30);
    }
    void stopTracking() { stopTimer(); trackPtr = nullptr; }

    void paint(juce::Graphics& g) override {
        g.setColour(juce::Colour(0xff555566));
        g.setFont(juce::FontOptions(9.0f));
        g.drawText(labelText, 0, getHeight() - kLabelH, getWidth(), kLabelH,
                   juce::Justification::centred);
    }

    void resized() override {
        slider.setBounds(0, 0, getWidth(), getHeight() - kLabelH);
    }

    static constexpr int kW = 44;  // preferred width
    static constexpr int kH = 56;  // preferred height (knob + label)
    static constexpr int kLabelH = 13;

private:
    void timerCallback() override {
        if (!trackPtr) return;
        float raw   = trackPtr->load(std::memory_order_relaxed);
        float range = trackMax - trackMin;
        float norm  = (range > 0.0f) ? (raw - trackMin) / range : 0.5f;
        double mapped = slider.getRange().getStart()
                      + norm * (slider.getRange().getEnd() - slider.getRange().getStart());
        slider.setValue(mapped, juce::dontSendNotification);
    }

    juce::String        labelText;
    juce::Colour        accentCol;
    std::atomic<float>* trackPtr { nullptr };
    float               trackMin { 0.0f };
    float               trackMax { 1.0f };
};
