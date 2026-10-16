#pragma once

#include "ControlPanel.h"
#include "StepGrid.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>

namespace splinter
{

class SplinterAudioProcessor;

/** Everything the editor shows, laid out at a fixed logical size. The editor scales it. */
class SplinterView final : public juce::Component
{
public:
    explicit SplinterView (SplinterAudioProcessor& processor);
    ~SplinterView() override;

    static constexpr int logicalWidth = 960;
    static constexpr int logicalHeight = 560;

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    void showPlayhead (int step, EffectType soundingEffect);
    void paintHeader (juce::Graphics& g) const;
    void paintStatus (juce::Graphics& g) const;

    StepGrid stepGrid_;

    ControlPanel globalPanel_;
    ControlPanel stutterPanel_;
    ControlPanel reversePanel_;
    ControlPanel tapeStopPanel_;
    ControlPanel stretchPanel_;
    ControlPanel crusherPanel_;
    ControlPanel gatePanel_;
    std::array<ControlPanel*, 6> effectPanels_;

    juce::TooltipWindow tooltipWindow_ { this, 900 };

    juce::Rectangle<int> headerArea_, statusArea_, patternArea_;
    int playheadStep_ = -1;
    EffectType soundingEffect_ = EffectType::None;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SplinterView)
};

} // namespace splinter
