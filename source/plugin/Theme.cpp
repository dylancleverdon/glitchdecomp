#include "Theme.h"

#include <algorithm>
#include <cmath>

namespace splinter::Theme
{

juce::Colour effectColour (EffectType type) noexcept
{
    switch (type)
    {
        case EffectType::None:     return juce::Colour (0xff3a3f48);
        case EffectType::Stutter:  return juce::Colour (0xffffb020); // amber
        case EffectType::Reverse:  return juce::Colour (0xff36d6a0); // mint
        case EffectType::TapeStop: return juce::Colour (0xffff6b5e); // coral
        case EffectType::Stretch:  return juce::Colour (0xff9483ff); // violet
        case EffectType::Crusher:  return juce::Colour (0xffff5fb0); // pink
        case EffectType::Gate:     return juce::Colour (0xff3fb4ff); // sky
    }

    return juce::Colour (0xff3a3f48);
}

const char* effectShortLabel (EffectType type) noexcept
{
    switch (type)
    {
        case EffectType::None:     return "OFF";
        case EffectType::Stutter:  return "STUT";
        case EffectType::Reverse:  return "REV";
        case EffectType::TapeStop: return "TAPE";
        case EffectType::Stretch:  return "STRCH";
        case EffectType::Crusher:  return "CRUSH";
        case EffectType::Gate:     return "GATE";
    }

    return "OFF";
}

juce::Font font (float height, bool bold)
{
    return juce::Font (juce::FontOptions (height, bold ? juce::Font::bold : juce::Font::plain));
}

//==============================================================================
namespace
{
    juce::Path repeatsGlyph (juce::Rectangle<float> area)
    {
        juce::Path path;
        constexpr int numBars = 4;
        const float barWidth = area.getWidth() / (numBars * 2 - 1);

        for (int bar = 0; bar < numBars; ++bar)
            path.addRoundedRectangle (area.getX() + static_cast<float> (bar * 2) * barWidth, area.getY() + area.getHeight() * 0.2f,
                                      barWidth, area.getHeight() * 0.6f, barWidth * 0.4f);

        return path;
    }

    juce::Path arrowGlyph (juce::Rectangle<float> area, bool pointsLeft, bool pointsRight)
    {
        juce::Path path;
        const float midY = area.getCentreY();
        const float head = area.getHeight() * 0.32f;

        path.startNewSubPath (area.getX(), midY);
        path.lineTo (area.getRight(), midY);

        if (pointsLeft)
        {
            path.startNewSubPath (area.getX() + head, midY - head);
            path.lineTo (area.getX(), midY);
            path.lineTo (area.getX() + head, midY + head);
        }

        if (pointsRight)
        {
            path.startNewSubPath (area.getRight() - head, midY - head);
            path.lineTo (area.getRight(), midY);
            path.lineTo (area.getRight() - head, midY + head);
        }

        return path;
    }

    // A wave that slows down and dies away, like a tape coming to a halt.
    juce::Path slowingWaveGlyph (juce::Rectangle<float> area)
    {
        juce::Path path;
        constexpr int numPoints = 48;
        double phase = 0.0;

        for (int point = 0; point <= numPoints; ++point)
        {
            const double progress = static_cast<double> (point) / numPoints;
            const double amplitude = std::sqrt (1.0 - progress);
            const auto x = area.getX() + static_cast<float> (progress) * area.getWidth();
            const auto y = area.getCentreY() - static_cast<float> (std::sin (phase) * amplitude) * area.getHeight() * 0.45f;

            if (point == 0)
                path.startNewSubPath (x, y);
            else
                path.lineTo (x, y);

            phase += 0.75 * (1.0 - progress);
        }

        return path;
    }

    juce::Path staircaseGlyph (juce::Rectangle<float> area)
    {
        juce::Path path;
        constexpr int numSteps = 5;
        const float stepWidth = area.getWidth() / numSteps;
        const float stepHeight = area.getHeight() / numSteps;

        path.startNewSubPath (area.getBottomLeft());

        for (int step = 0; step < numSteps; ++step)
        {
            const auto y = area.getBottom() - static_cast<float> (step + 1) * stepHeight;
            path.lineTo (area.getX() + static_cast<float> (step) * stepWidth, y);
            path.lineTo (area.getX() + static_cast<float> (step + 1) * stepWidth, y);
        }

        return path;
    }

    juce::Path pulseTrainGlyph (juce::Rectangle<float> area)
    {
        juce::Path path;
        constexpr int numPulses = 3;
        const float cycle = area.getWidth() / numPulses;
        const float top = area.getY() + area.getHeight() * 0.15f;
        const float bottom = area.getBottom() - area.getHeight() * 0.15f;

        path.startNewSubPath (area.getX(), bottom);

        for (int pulse = 0; pulse < numPulses; ++pulse)
        {
            const float start = area.getX() + static_cast<float> (pulse) * cycle + cycle * 0.15f;
            const float end = start + cycle * 0.5f;
            path.lineTo (start, bottom);
            path.lineTo (start, top);
            path.lineTo (end, top);
            path.lineTo (end, bottom);
        }

        path.lineTo (area.getRight(), bottom);
        return path;
    }
}

void drawEffectGlyph (juce::Graphics& g, EffectType type, juce::Rectangle<float> area, juce::Colour colour)
{
    g.setColour (colour);
    const juce::PathStrokeType stroke (std::max (1.2f, area.getHeight() * 0.09f), juce::PathStrokeType::curved, juce::PathStrokeType::rounded);

    switch (type)
    {
        case EffectType::None:
            g.fillEllipse (juce::Rectangle<float> (4.0f, 4.0f).withCentre (area.getCentre()));
            break;

        case EffectType::Stutter:  g.fillPath (repeatsGlyph (area)); break;
        case EffectType::Reverse:  g.strokePath (arrowGlyph (area, true, false), stroke); break;
        case EffectType::TapeStop: g.strokePath (slowingWaveGlyph (area), stroke); break;
        case EffectType::Stretch:  g.strokePath (arrowGlyph (area, true, true), stroke); break;
        case EffectType::Crusher:  g.strokePath (staircaseGlyph (area), stroke); break;
        case EffectType::Gate:     g.strokePath (pulseTrainGlyph (area), stroke); break;
    }
}

} // namespace splinter::Theme
