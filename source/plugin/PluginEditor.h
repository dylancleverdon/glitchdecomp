#pragma once

#include "SplinterLookAndFeel.h"
#include "SplinterView.h"

#include <juce_audio_processors/juce_audio_processors.h>

namespace splinter
{

class SplinterAudioProcessor;

/** The plugin window: hosts SplinterView and scales it to the window, keeping its aspect ratio. */
class SplinterEditor final : public juce::AudioProcessorEditor
{
public:
    explicit SplinterEditor (SplinterAudioProcessor& pluginProcessor);
    ~SplinterEditor() override;

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    SplinterLookAndFeel lookAndFeel_; // declared first: it must outlive every component using it
    SplinterView view_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SplinterEditor)
};

} // namespace splinter
