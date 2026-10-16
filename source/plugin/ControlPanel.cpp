#include "ControlPanel.h"

#include "SplinterLookAndFeel.h"
#include "Theme.h"

#include <algorithm>

namespace splinter
{

namespace
{
    constexpr float cornerSize = 8.0f;
    constexpr int labelHeight = 18;
    constexpr int valueBoxHeight = 20;
    constexpr int comboHeight = 28;
    constexpr int maxComboWidth = 100;
}

struct ControlPanel::Control
{
    // Declared before the attachments, so the attachments are destroyed first.
    juce::Label label;
    std::unique_ptr<juce::Slider> slider;
    std::unique_ptr<juce::ComboBox> comboBox;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> sliderAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> comboBoxAttachment;
};

//==============================================================================
ControlPanel::ControlPanel (juce::String title, juce::Colour accent, std::optional<EffectType> effect)
    : title_ (std::move (title)), accent_ (accent), effect_ (effect)
{
    setTitle (title_);
}

ControlPanel::~ControlPanel() = default;

ControlPanel::Control& ControlPanel::addControl (const juce::String& label)
{
    auto& control = *controls_.emplace_back (std::make_unique<Control>());

    control.label.setText (label, juce::dontSendNotification);
    control.label.setJustificationType (juce::Justification::centred);
    control.label.setFont (Theme::font (12.5f));
    control.label.setColour (juce::Label::textColourId, Theme::textDim);
    control.label.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (control.label);

    return control;
}

void ControlPanel::addKnob (juce::AudioProcessorValueTreeState& state, const juce::String& parameterID, const juce::String& label,
                            const juce::String& tooltip, std::optional<float> arcOrigin)
{
    auto* parameter = state.getParameter (parameterID);
    jassert (parameter != nullptr);

    if (parameter == nullptr)
        return;

    auto& control = addControl (label);
    auto& slider = *(control.slider = std::make_unique<juce::Slider> (juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow));

    slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 84, valueBoxHeight);
    slider.setColour (juce::Slider::rotarySliderFillColourId, accent_);
    slider.setColour (juce::Slider::textBoxHighlightColourId, accent_.withAlpha (0.35f));
    slider.setTitle (parameter->getName (64));
    slider.setTooltip (tooltip);

    if (arcOrigin.has_value())
        slider.getProperties().set (SplinterLookAndFeel::arcOriginProperty, *arcOrigin);

    addAndMakeVisible (slider);
    control.sliderAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (state, parameterID, slider);

    // After the attachment, which installs the parameter's own text conversion.
    const auto unit = parameter->getLabel();
    slider.setTextValueSuffix (unit.isNotEmpty() ? " " + unit : juce::String());

    if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (parameter))
        slider.setDoubleClickReturnValue (true, ranged->convertFrom0to1 (ranged->getDefaultValue()));
}

void ControlPanel::addChoice (juce::AudioProcessorValueTreeState& state, const juce::String& parameterID, const juce::String& label,
                              const juce::String& tooltip)
{
    auto* parameter = dynamic_cast<juce::AudioParameterChoice*> (state.getParameter (parameterID));
    jassert (parameter != nullptr);

    if (parameter == nullptr)
        return;

    auto& control = addControl (label);
    auto& comboBox = *(control.comboBox = std::make_unique<juce::ComboBox>());

    // The attachment maps item IDs 1, 2, 3 ... to choice indices 0, 1, 2 ...
    comboBox.addItemList (parameter->choices, 1);
    comboBox.setJustificationType (juce::Justification::centred);
    comboBox.setColour (juce::ComboBox::arrowColourId, accent_);
    comboBox.setColour (juce::ComboBox::focusedOutlineColourId, accent_);
    comboBox.setTitle (parameter->getName (64));
    comboBox.setTooltip (tooltip);

    addAndMakeVisible (comboBox);
    control.comboBoxAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (state, parameterID, comboBox);
}

void ControlPanel::setLit (bool shouldBeLit)
{
    if (isLit_ == shouldBeLit)
        return;

    isLit_ = shouldBeLit;
    repaint (getLocalBounds().removeFromTop (headerHeight));
}

//==============================================================================
void ControlPanel::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat().reduced (0.5f);

    g.setColour (Theme::panel);
    g.fillRoundedRectangle (bounds, cornerSize);

    auto headerBounds = bounds.removeFromTop (static_cast<float> (headerHeight));
    juce::Path header;
    header.addRoundedRectangle (headerBounds.getX(), headerBounds.getY(), headerBounds.getWidth(), headerBounds.getHeight(),
                                cornerSize, cornerSize, true, true, false, false);

    g.setColour (accent_.withAlpha (isLit_ ? 0.92f : 0.13f));
    g.fillPath (header);

    g.setColour (accent_.withAlpha (isLit_ ? 1.0f : 0.55f));
    g.fillRect (headerBounds.removeFromBottom (1.0f));

    g.setColour (Theme::panelOutline);
    g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (0.5f), cornerSize, 1.0f);

    const auto ink = isLit_ ? Theme::ink : accent_;
    auto titleArea = headerBounds.reduced (10.0f, 0.0f);

    if (effect_.has_value())
    {
        const auto glyphArea = titleArea.removeFromLeft (18.0f).withSizeKeepingCentre (18.0f, 11.0f);
        Theme::drawEffectGlyph (g, *effect_, glyphArea, ink);
        titleArea.removeFromLeft (7.0f);
    }

    g.setColour (ink);
    g.setFont (Theme::font (12.5f, true).withExtraKerningFactor (0.12f));
    g.drawText (title_.toUpperCase(), titleArea, juce::Justification::centredLeft, true);
}

void ControlPanel::resized()
{
    if (controls_.empty())
        return;

    auto area = getLocalBounds().withTrimmedTop (headerHeight).reduced (4, 8);
    const int slotWidth = area.getWidth() / getNumControls();

    for (size_t index = 0; index < controls_.size(); ++index)
    {
        auto& control = *controls_[index];
        auto slot = index + 1 < controls_.size() ? area.removeFromLeft (slotWidth) : area;

        control.label.setBounds (slot.removeFromTop (labelHeight));

        if (control.slider != nullptr)
            control.slider->setBounds (slot);

        if (control.comboBox != nullptr)
        {
            // Centred on the same line as the knobs' centres in neighbouring slots.
            const auto knobArea = slot.withTrimmedBottom (valueBoxHeight);
            const int width = std::min (slot.getWidth() - 12, maxComboWidth);
            control.comboBox->setBounds (knobArea.withSizeKeepingCentre (width, comboHeight));
        }
    }
}

} // namespace splinter
