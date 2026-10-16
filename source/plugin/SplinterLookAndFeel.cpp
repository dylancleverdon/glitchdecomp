#include "SplinterLookAndFeel.h"

#include "Theme.h"

#include <algorithm>
#include <cmath>

namespace splinter
{

SplinterLookAndFeel::SplinterLookAndFeel()
{
    setColourScheme ({ Theme::background,                   // window background
                       Theme::backgroundRaised,             // widget background
                       Theme::panel,                        // menu background
                       Theme::panelOutline,                 // outline
                       Theme::text,                         // default text
                       Theme::track,                        // default fill
                       Theme::text,                         // highlighted text
                       Theme::panelOutline.brighter (0.3f), // highlighted fill
                       Theme::text });                      // menu text

    setColour (juce::ResizableWindow::backgroundColourId, Theme::background);

    setColour (juce::Slider::rotarySliderFillColourId, Theme::globalAccent);
    setColour (juce::Slider::textBoxTextColourId, Theme::text);
    setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour (juce::Slider::textBoxHighlightColourId, Theme::globalAccent.withAlpha (0.35f));

    setColour (juce::Label::textColourId, Theme::text);
    setColour (juce::TextEditor::backgroundColourId, Theme::backgroundRaised);
    setColour (juce::TextEditor::focusedOutlineColourId, Theme::globalAccent);

    setColour (juce::ComboBox::backgroundColourId, Theme::backgroundRaised);
    setColour (juce::ComboBox::outlineColourId, Theme::panelOutline);
    setColour (juce::ComboBox::textColourId, Theme::text);
    setColour (juce::ComboBox::arrowColourId, Theme::globalAccent);
    setColour (juce::ComboBox::focusedOutlineColourId, Theme::globalAccent);

    setColour (juce::PopupMenu::backgroundColourId, Theme::panel);
    setColour (juce::PopupMenu::textColourId, Theme::text);
    setColour (juce::PopupMenu::headerTextColourId, Theme::textDim);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, Theme::panelOutline.brighter (0.25f));
    setColour (juce::PopupMenu::highlightedTextColourId, juce::Colours::white);

    setColour (juce::TooltipWindow::backgroundColourId, Theme::panel.brighter (0.08f));
    setColour (juce::TooltipWindow::textColourId, Theme::text);
    setColour (juce::TooltipWindow::outlineColourId, Theme::panelOutline);
}

//==============================================================================
void SplinterLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height, float sliderPosition,
                                            float rotaryStartAngle, float rotaryEndAngle, juce::Slider& slider)
{
    const auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat().reduced (2.0f);
    const float radius = std::min (bounds.getWidth(), bounds.getHeight()) * 0.5f;

    if (radius < 4.0f)
        return;

    const auto centre = bounds.getCentre();
    const float trackWidth = std::max (2.5f, radius * 0.1f);
    const float arcRadius = radius - trackWidth * 0.5f;
    const float angleRange = rotaryEndAngle - rotaryStartAngle;
    const float valueAngle = rotaryStartAngle + sliderPosition * angleRange;
    const juce::PathStrokeType arcStroke (trackWidth, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);

    auto accent = slider.findColour (juce::Slider::rotarySliderFillColourId);

    if (! slider.isEnabled())
        accent = accent.withMultipliedSaturation (0.2f).withMultipliedAlpha (0.5f);
    else if (slider.isMouseOverOrDragging())
        accent = accent.brighter (0.15f);

    juce::Path track;
    track.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f, rotaryStartAngle, rotaryEndAngle, true);
    g.setColour (Theme::track);
    g.strokePath (track, arcStroke);

    float originAngle = rotaryStartAngle;
    const auto& origin = slider.getProperties()[arcOriginProperty];

    if (! origin.isVoid())
        originAngle = rotaryStartAngle + static_cast<float> (slider.valueToProportionOfLength (static_cast<double> (origin))) * angleRange;

    if (std::abs (valueAngle - originAngle) > 0.001f)
    {
        juce::Path valueArc;
        valueArc.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f,
                                std::min (originAngle, valueAngle), std::max (originAngle, valueAngle), true);
        g.setColour (accent);
        g.strokePath (valueArc, arcStroke);
    }

    const float bodyRadius = arcRadius - trackWidth * 1.6f;
    const auto body = juce::Rectangle<float> (bodyRadius * 2.0f, bodyRadius * 2.0f).withCentre (centre);

    g.setGradientFill (juce::ColourGradient (Theme::knobBody.brighter (0.18f), body.getX(), body.getY(),
                                             Theme::knobBody.darker (0.35f), body.getX(), body.getBottom(), false));
    g.fillEllipse (body);
    g.setColour (Theme::panelOutline.brighter (0.15f));
    g.drawEllipse (body, 1.0f);

    juce::Path pointer;
    pointer.startNewSubPath (centre.getPointOnCircumference (bodyRadius * 0.3f, valueAngle));
    pointer.lineTo (centre.getPointOnCircumference (bodyRadius * 0.82f, valueAngle));
    g.setColour (slider.isEnabled() ? Theme::text : Theme::textDim);
    g.strokePath (pointer, juce::PathStrokeType (std::max (2.0f, radius * 0.07f), juce::PathStrokeType::curved,
                                                 juce::PathStrokeType::rounded));
}

juce::Label* SplinterLookAndFeel::createSliderTextBox (juce::Slider& slider)
{
    auto* label = LookAndFeel_V4::createSliderTextBox (slider);
    label->setFont (Theme::font (13.0f));
    return label;
}

//==============================================================================
void SplinterLookAndFeel::drawComboBox (juce::Graphics& g, int width, int height, bool,
                                        int, int, int, int, juce::ComboBox& box)
{
    const auto bounds = juce::Rectangle<int> (width, height).toFloat().reduced (0.5f);
    const float cornerSize = 4.0f;
    const auto accent = box.findColour (juce::ComboBox::arrowColourId);

    g.setColour (box.findColour (juce::ComboBox::backgroundColourId));
    g.fillRoundedRectangle (bounds, cornerSize);

    const bool highlighted = box.isEnabled() && (box.isMouseOver (true) || box.isPopupActive());
    g.setColour (highlighted ? accent.withAlpha (0.7f) : box.findColour (juce::ComboBox::outlineColourId));
    g.drawRoundedRectangle (bounds, cornerSize, 1.0f);

    const auto arrowCentre = juce::Point<float> (bounds.getRight() - 12.0f, bounds.getCentreY());
    juce::Path chevron;
    chevron.startNewSubPath (arrowCentre.translated (-4.0f, -2.0f));
    chevron.lineTo (arrowCentre.translated (0.0f, 2.0f));
    chevron.lineTo (arrowCentre.translated (4.0f, -2.0f));

    g.setColour (box.isEnabled() ? accent : accent.withAlpha (0.3f));
    g.strokePath (chevron, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

juce::Font SplinterLookAndFeel::getComboBoxFont (juce::ComboBox&)
{
    return Theme::font (14.0f);
}

void SplinterLookAndFeel::positionComboBoxText (juce::ComboBox& box, juce::Label& label)
{
    // Leave room for the chevron on the right; the text is centred in the rest.
    label.setBounds (4, 1, box.getWidth() - 24, box.getHeight() - 2);
    label.setFont (getComboBoxFont (box));
}

juce::Font SplinterLookAndFeel::getPopupMenuFont()
{
    return Theme::font (14.0f);
}

} // namespace splinter
