#pragma once

#include "dsp/EffectParams.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <atomic>

namespace splinter
{

/** Parameter IDs. They are stored in presets and host sessions: only append, never rename. */
namespace ParamIDs
{
    inline constexpr const char* mix = "mix";
    inline constexpr const char* output = "output";
    inline constexpr const char* stepRate = "stepRate";
    inline constexpr const char* steps = "steps";
    inline constexpr const char* chance = "chance";
    inline constexpr const char* stutterRepeat = "stutterRepeat";
    inline constexpr const char* stutterPitch = "stutterPitch";
    inline constexpr const char* stutterDecay = "stutterDecay";
    inline constexpr const char* reverseSpeed = "reverseSpeed";
    inline constexpr const char* tapeStopTime = "tapeStopTime";
    inline constexpr const char* tapeStopCurve = "tapeStopCurve";
    inline constexpr const char* stretchFactor = "stretchFactor";
    inline constexpr const char* stretchGrain = "stretchGrain";
    inline constexpr const char* crusherBits = "crusherBits";
    inline constexpr const char* crusherDownsample = "crusherDownsample";
    inline constexpr const char* gateDivision = "gateDivision";
    inline constexpr const char* gateDuty = "gateDuty";

    /** "step1" ... "step16" for stepIndex 0 ... 15. */
    juce::String step (int stepIndex);
}

/** Version hint passed to every juce::ParameterID. */
inline constexpr int parameterVersionHint = 1;

/** Choice lists. The order is stored in presets: only append. */
namespace ParamChoices
{
    const juce::StringArray& stepRates();     // 1/4 ... 1/32
    const juce::StringArray& noteDivisions(); // 1/8 ... 1/128, shared by Stutter Repeat and Gate Division
    const juce::StringArray& effects();       // Off, Stutter, ... in EffectType order
}

/** Length in beats of each ParamChoices::stepRates() entry. */
inline constexpr std::array<double, 6> stepRateBeats { 1.0, 0.5, 1.0 / 3.0, 0.25, 1.0 / 6.0, 0.125 };

/** Length in beats of each ParamChoices::noteDivisions() entry. */
inline constexpr std::array<float, 5> noteDivisionBeats { 0.5f, 0.25f, 0.125f, 0.0625f, 0.03125f };

inline constexpr int defaultStepRateIndex = 3;     // 1/16
inline constexpr int defaultNoteDivisionIndex = 2; // 1/32

inline constexpr std::array<EffectType, maxSteps> defaultPattern {
    EffectType::None,    EffectType::None,    EffectType::Stutter,  EffectType::None,
    EffectType::None,    EffectType::None,    EffectType::Reverse,  EffectType::None,
    EffectType::None,    EffectType::Crusher, EffectType::None,     EffectType::Gate,
    EffectType::Stretch, EffectType::Stretch, EffectType::TapeStop, EffectType::TapeStop,
};

juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

/** Reads the current parameter values into EngineParams on the audio thread.

    The raw value pointers are looked up once on construction, so read() does no
    string lookups, allocation or locking. */
class ParameterReader
{
public:
    explicit ParameterReader (juce::AudioProcessorValueTreeState& state);

    EngineParams read() const noexcept;

private:
    std::atomic<float>* mix_ = nullptr;
    std::atomic<float>* output_ = nullptr;
    std::atomic<float>* stepRate_ = nullptr;
    std::atomic<float>* steps_ = nullptr;
    std::atomic<float>* chance_ = nullptr;
    std::array<std::atomic<float>*, maxSteps> stepEffects_ {};
    std::atomic<float>* stutterRepeat_ = nullptr;
    std::atomic<float>* stutterPitch_ = nullptr;
    std::atomic<float>* stutterDecay_ = nullptr;
    std::atomic<float>* reverseSpeed_ = nullptr;
    std::atomic<float>* tapeStopTime_ = nullptr;
    std::atomic<float>* tapeStopCurve_ = nullptr;
    std::atomic<float>* stretchFactor_ = nullptr;
    std::atomic<float>* stretchGrain_ = nullptr;
    std::atomic<float>* crusherBits_ = nullptr;
    std::atomic<float>* crusherDownsample_ = nullptr;
    std::atomic<float>* gateDivision_ = nullptr;
    std::atomic<float>* gateDuty_ = nullptr;
};

} // namespace splinter
