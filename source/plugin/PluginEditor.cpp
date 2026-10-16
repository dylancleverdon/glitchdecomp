#include "PluginEditor.h"

#include "PluginProcessor.h"
#include "Theme.h"

#include <algorithm>

namespace splinter
{

SplinterEditor::SplinterEditor (SplinterAudioProcessor& pluginProcessor)
    : AudioProcessorEditor (pluginProcessor),
      view_ (pluginProcessor)
{
    // Set after adding the view, so the change reaches every child and their text boxes pick up its colours.
    addAndMakeVisible (view_);
    setLookAndFeel (&lookAndFeel_);

    constexpr int width = SplinterView::logicalWidth;
    constexpr int height = SplinterView::logicalHeight;

    // Added after the view, so the corner resizer sits on top of it.
    setResizable (true, true);
    setResizeLimits (width * 3 / 4, height * 3 / 4, width * 2, height * 2);
    getConstrainer()->setFixedAspectRatio (static_cast<double> (width) / static_cast<double> (height));
    setSize (width, height);
}

SplinterEditor::~SplinterEditor()
{
    setLookAndFeel (nullptr);
}

void SplinterEditor::paint (juce::Graphics& g)
{
    // Only visible if a host forces a size that does not match the aspect ratio.
    g.fillAll (Theme::background);
}

void SplinterEditor::resized()
{
    constexpr auto width = static_cast<float> (SplinterView::logicalWidth);
    constexpr auto height = static_cast<float> (SplinterView::logicalHeight);

    const float scale = std::min (static_cast<float> (getWidth()) / width, static_cast<float> (getHeight()) / height);
    const float offsetX = (static_cast<float> (getWidth()) - width * scale) * 0.5f;
    const float offsetY = (static_cast<float> (getHeight()) - height * scale) * 0.5f;

    view_.setBounds (0, 0, SplinterView::logicalWidth, SplinterView::logicalHeight);
    view_.setTransform (juce::AffineTransform::scale (scale).translated (offsetX, offsetY));
}

} // namespace splinter
