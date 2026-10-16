#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace splinter
{

/** Flat dark look: arc knobs in each control's accent colour, rounded combo boxes. */
class SplinterLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    SplinterLookAndFeel();

    /** Slider property: the value the value arc starts from (defaults to the minimum).
        Set it to 0 on bipolar controls so the arc grows outwards from the centre. */
    static inline const juce::Identifier arcOriginProperty { "arcOrigin" };

    void drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height, float sliderPosition,
                           float rotaryStartAngle, float rotaryEndAngle, juce::Slider& slider) override;
    juce::Label* createSliderTextBox (juce::Slider& slider) override;

    void drawComboBox (juce::Graphics& g, int width, int height, bool isButtonDown,
                       int buttonX, int buttonY, int buttonWidth, int buttonHeight, juce::ComboBox& box) override;
    juce::Font getComboBoxFont (juce::ComboBox& box) override;
    void positionComboBoxText (juce::ComboBox& box, juce::Label& label) override;

    juce::Font getPopupMenuFont() override;
};

} // namespace splinter
