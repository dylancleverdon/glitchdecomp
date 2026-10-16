#pragma once

#include "dsp/EffectParams.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <memory>
#include <optional>
#include <vector>

namespace splinter
{

/** A titled group of labelled controls in one row, each bound to a parameter.

    The header is drawn in the panel's accent colour; for an effect panel that is the
    effect's colour from the step grid, and the header lights up while the effect sounds. */
class ControlPanel final : public juce::Component
{
public:
    /** effect is the effect this panel controls, or nullopt for a non-effect panel. */
    ControlPanel (juce::String title, juce::Colour accent, std::optional<EffectType> effect = std::nullopt);
    ~ControlPanel() override;

    /** Adds a rotary knob. arcOrigin is where the value arc starts (e.g. 0 for a bipolar control). */
    void addKnob (juce::AudioProcessorValueTreeState& state, const juce::String& parameterID, const juce::String& label,
                  const juce::String& tooltip, std::optional<float> arcOrigin = std::nullopt);

    /** Adds a combo box listing the choices of an AudioParameterChoice. */
    void addChoice (juce::AudioProcessorValueTreeState& state, const juce::String& parameterID, const juce::String& label,
                    const juce::String& tooltip);

    int getNumControls() const noexcept { return static_cast<int> (controls_.size()); }

    /** Share of a row this panel should get. A lone control still leaves room for the title. */
    float getLayoutWeight() const noexcept { return std::max (static_cast<float> (getNumControls()), 1.5f); }

    std::optional<EffectType> getEffect() const noexcept { return effect_; }

    /** Lights the header while the panel's effect is sounding. */
    void setLit (bool shouldBeLit);

    void paint (juce::Graphics& g) override;
    void resized() override;

    static constexpr int headerHeight = 26;

private:
    struct Control;

    Control& addControl (const juce::String& label);

    juce::String title_;
    juce::Colour accent_;
    std::optional<EffectType> effect_;
    bool isLit_ = false;
    std::vector<std::unique_ptr<Control>> controls_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ControlPanel)
};

} // namespace splinter
