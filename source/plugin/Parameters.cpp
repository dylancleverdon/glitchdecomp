#include "Parameters.h"

#include <cmath>

namespace splinter
{

juce::String ParamIDs::step (int stepIndex)
{
    return "step" + juce::String (stepIndex + 1);
}

const juce::StringArray& ParamChoices::stepRates()
{
    static const juce::StringArray choices { "1/4", "1/8", "1/8T", "1/16", "1/16T", "1/32" };
    return choices;
}

const juce::StringArray& ParamChoices::noteDivisions()
{
    static const juce::StringArray choices { "1/8", "1/16", "1/32", "1/64", "1/128" };
    return choices;
}

const juce::StringArray& ParamChoices::effects()
{
    static const juce::StringArray choices = []
    {
        juce::StringArray names;

        for (int type = 0; type < numEffectTypes; ++type)
            names.add (effectTypeName (static_cast<EffectType> (type)));

        return names;
    }();

    return choices;
}

//==============================================================================
namespace
{
    using StringFromValue = std::function<juce::String (float value, int maximumStringLength)>;

    // Reads the leading number of a typed value such as "-6 dB" or "50%".
    float parseNumber (const juce::String& text, float fallback)
    {
        const auto value = text.trim().getFloatValue();
        return std::isfinite (value) ? value : fallback;
    }

    juce::String formatFixed (float value, int decimals)
    {
        // Anything that rounds to zero is shown as an unsigned zero rather than "-0.0".
        if (std::abs (value) < 0.5f * std::pow (10.0f, static_cast<float> (-decimals)))
            return juce::String (0.0, decimals);

        return juce::String (value, decimals);
    }

    juce::String formatSigned (float value, int decimals)
    {
        const auto text = formatFixed (value, decimals);
        return text.getDoubleValue() > 0.0 ? "+" + text : text;
    }

    StringFromValue fixedDecimals (int decimals)
    {
        return [decimals] (float value, int) { return formatFixed (value, decimals); };
    }

    StringFromValue signedDecimals (int decimals)
    {
        return [decimals] (float value, int) { return formatSigned (value, decimals); };
    }

    StringFromValue wholeNumber()
    {
        return [] (float value, int) { return juce::String (juce::roundToInt (value)); };
    }

    juce::NormalisableRange<float> skewedRange (float minimum, float maximum, float interval, float centre)
    {
        juce::NormalisableRange<float> range (minimum, maximum, interval);
        range.setSkewForCentre (centre);
        return range;
    }

    std::unique_ptr<juce::AudioParameterFloat> makeFloat (const char* id,
                                                          const char* name,
                                                          juce::NormalisableRange<float> range,
                                                          float defaultValue,
                                                          const char* label,
                                                          StringFromValue format)
    {
        auto attributes = juce::AudioParameterFloatAttributes()
                              .withLabel (label)
                              .withStringFromValueFunction (std::move (format))
                              .withValueFromStringFunction ([defaultValue] (const juce::String& text)
                                                            { return parseNumber (text, defaultValue); });

        return std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { id, parameterVersionHint },
                                                            name,
                                                            range,
                                                            defaultValue,
                                                            attributes);
    }

    std::unique_ptr<juce::AudioParameterFloat> makePercent (const char* id, const char* name, float minimum, float maximum, float defaultValue)
    {
        return makeFloat (id, name, { minimum, maximum, 0.1f }, defaultValue, "%", wholeNumber());
    }

    std::unique_ptr<juce::AudioParameterChoice> makeChoice (const char* id, const char* name, const juce::StringArray& choices, int defaultIndex)
    {
        return std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { id, parameterVersionHint }, name, choices, defaultIndex);
    }

    template <typename... Parameters>
    std::unique_ptr<juce::AudioProcessorParameterGroup> makeGroup (const char* id, const char* name, Parameters... parameters)
    {
        return std::make_unique<juce::AudioProcessorParameterGroup> (id, name, "|", std::move (parameters)...);
    }

    float load (const std::atomic<float>* value) noexcept
    {
        return value->load (std::memory_order_relaxed);
    }

    template <typename Table>
    auto choiceValue (const Table& table, const std::atomic<float>* rawIndex) noexcept
    {
        const auto index = juce::jlimit (0, static_cast<int> (table.size()) - 1, juce::roundToInt (load (rawIndex)));
        return table[static_cast<size_t> (index)];
    }
}

//==============================================================================
juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (makeGroup ("global", "Global",
                           makePercent (ParamIDs::mix, "Mix", 0.0f, 100.0f, 100.0f),
                           makeFloat (ParamIDs::output, "Output", { -24.0f, 12.0f, 0.1f }, 0.0f, "dB", signedDecimals (1)),
                           makeChoice (ParamIDs::stepRate, "Step Rate", ParamChoices::stepRates(), defaultStepRateIndex),
                           std::make_unique<juce::AudioParameterInt> (juce::ParameterID { ParamIDs::steps, parameterVersionHint },
                                                                      "Steps", 1, maxSteps, maxSteps),
                           makePercent (ParamIDs::chance, "Chance", 0.0f, 100.0f, 100.0f)));

    auto pattern = std::make_unique<juce::AudioProcessorParameterGroup> ("pattern", "Pattern", "|");

    for (int stepIndex = 0; stepIndex < maxSteps; ++stepIndex)
    {
        const auto defaultIndex = static_cast<int> (defaultPattern[static_cast<size_t> (stepIndex)]);
        pattern->addChild (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { ParamIDs::step (stepIndex), parameterVersionHint },
                                                                         "Step " + juce::String (stepIndex + 1),
                                                                         ParamChoices::effects(),
                                                                         defaultIndex));
    }

    layout.add (std::move (pattern));

    layout.add (makeGroup ("stutter", "Stutter",
                           makeChoice (ParamIDs::stutterRepeat, "Stutter Repeat", ParamChoices::noteDivisions(), defaultNoteDivisionIndex),
                           makeFloat (ParamIDs::stutterPitch, "Stutter Pitch", { -24.0f, 24.0f, 0.1f }, 0.0f, "st", signedDecimals (1)),
                           makePercent (ParamIDs::stutterDecay, "Stutter Decay", 0.0f, 100.0f, 0.0f)));

    layout.add (makeGroup ("reverse", "Reverse",
                           makeFloat (ParamIDs::reverseSpeed, "Reverse Speed", skewedRange (0.5f, 2.0f, 0.01f, 1.0f), 1.0f, "x", fixedDecimals (2))));

    layout.add (makeGroup ("tapeStop", "Tape Stop",
                           makePercent (ParamIDs::tapeStopTime, "Tape Stop Time", 5.0f, 100.0f, 100.0f),
                           makePercent (ParamIDs::tapeStopCurve, "Tape Stop Curve", 0.0f, 100.0f, 50.0f)));

    layout.add (makeGroup ("stretch", "Stretch",
                           makeFloat (ParamIDs::stretchFactor, "Stretch Factor", skewedRange (1.0f, 16.0f, 0.01f, 4.0f), 4.0f, "x", fixedDecimals (2)),
                           makeFloat (ParamIDs::stretchGrain, "Stretch Grain", skewedRange (10.0f, 250.0f, 0.1f, 80.0f), 80.0f, "ms", wholeNumber())));

    layout.add (makeGroup ("crusher", "Crusher",
                           makeFloat (ParamIDs::crusherBits, "Crusher Bits", { 1.0f, 16.0f, 0.01f }, 6.0f, "bits", fixedDecimals (1)),
                           makeFloat (ParamIDs::crusherDownsample, "Crusher Downsample", skewedRange (1.0f, 64.0f, 0.01f, 8.0f), 8.0f, "x", fixedDecimals (1))));

    layout.add (makeGroup ("gate", "Gate",
                           makeChoice (ParamIDs::gateDivision, "Gate Division", ParamChoices::noteDivisions(), defaultNoteDivisionIndex),
                           makePercent (ParamIDs::gateDuty, "Gate Duty", 5.0f, 95.0f, 50.0f)));

    return layout;
}

//==============================================================================
ParameterReader::ParameterReader (juce::AudioProcessorValueTreeState& state)
{
    auto lookUp = [&state] (const juce::String& id)
    {
        auto* value = state.getRawParameterValue (id);
        jassert (value != nullptr); // every ID used here must be in createParameterLayout()
        return value;
    };

    mix_ = lookUp (ParamIDs::mix);
    output_ = lookUp (ParamIDs::output);
    stepRate_ = lookUp (ParamIDs::stepRate);
    steps_ = lookUp (ParamIDs::steps);
    chance_ = lookUp (ParamIDs::chance);

    for (size_t stepIndex = 0; stepIndex < stepEffects_.size(); ++stepIndex)
        stepEffects_[stepIndex] = lookUp (ParamIDs::step (static_cast<int> (stepIndex)));

    stutterRepeat_ = lookUp (ParamIDs::stutterRepeat);
    stutterPitch_ = lookUp (ParamIDs::stutterPitch);
    stutterDecay_ = lookUp (ParamIDs::stutterDecay);
    reverseSpeed_ = lookUp (ParamIDs::reverseSpeed);
    tapeStopTime_ = lookUp (ParamIDs::tapeStopTime);
    tapeStopCurve_ = lookUp (ParamIDs::tapeStopCurve);
    stretchFactor_ = lookUp (ParamIDs::stretchFactor);
    stretchGrain_ = lookUp (ParamIDs::stretchGrain);
    crusherBits_ = lookUp (ParamIDs::crusherBits);
    crusherDownsample_ = lookUp (ParamIDs::crusherDownsample);
    gateDivision_ = lookUp (ParamIDs::gateDivision);
    gateDuty_ = lookUp (ParamIDs::gateDuty);
}

EngineParams ParameterReader::read() const noexcept
{
    static constexpr float percent = 0.01f;

    EngineParams params;
    params.mix = load (mix_) * percent;
    params.outputGainDb = load (output_);

    auto& sequencer = params.sequencer;
    sequencer.numSteps = juce::jlimit (1, maxSteps, juce::roundToInt (load (steps_)));
    sequencer.stepBeats = choiceValue (stepRateBeats, stepRate_);
    sequencer.chance = load (chance_) * percent;

    for (size_t stepIndex = 0; stepIndex < stepEffects_.size(); ++stepIndex)
    {
        const auto type = juce::jlimit (0, numEffectTypes - 1, juce::roundToInt (load (stepEffects_[stepIndex])));
        sequencer.steps[stepIndex] = static_cast<EffectType> (type);
    }

    params.stutter.repeatBeats = choiceValue (noteDivisionBeats, stutterRepeat_);
    params.stutter.pitchGlideSemitones = load (stutterPitch_);
    params.stutter.decay = load (stutterDecay_) * percent;

    params.reverse.speed = load (reverseSpeed_);

    params.tapeStop.duration = load (tapeStopTime_) * percent;
    params.tapeStop.curve = load (tapeStopCurve_) * percent;

    params.stretch.factor = load (stretchFactor_);
    params.stretch.grainMs = load (stretchGrain_);

    params.crusher.bits = load (crusherBits_);
    params.crusher.downsample = load (crusherDownsample_);

    params.gate.divisionBeats = choiceValue (noteDivisionBeats, gateDivision_);
    params.gate.duty = load (gateDuty_) * percent;

    return params;
}

} // namespace splinter
