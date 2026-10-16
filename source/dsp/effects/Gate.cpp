#include "Gate.h"

#include <algorithm>
#include <cmath>

namespace splinter
{

namespace
{
    constexpr double pi = 3.14159265358979323846;

    /** 0 -> 1 with zero slope at both ends. */
    double raisedCosine (double x) noexcept
    {
        return 0.5 - 0.5 * std::cos (pi * std::clamp (x, 0.0, 1.0));
    }
} // namespace

void Gate::prepare (double sampleRate, int /*maxBlockSize*/)
{
    sampleRate_ = sampleRate > 0.0 && std::isfinite (sampleRate) ? sampleRate : 44100.0;
    reset();
}

void Gate::reset() noexcept
{
    hasPendingTrigger_ = false;
    hasPattern_ = false;
    pattern_ = {};
    lastGain_ = 0.0f;
    processedEnd_ = std::numeric_limits<int64_t>::min();
}

void Gate::trigger (const StepContext& step) noexcept
{
    pendingStep_ = step;
    hasPendingTrigger_ = true;
}

Gate::Pattern Gate::makePattern (const StepContext& step) const noexcept
{
    const bool tempoValid = std::isfinite (step.samplesPerBeat) && step.samplesPerBeat > 0.0;
    const double samplesPerBeat = tempoValid ? step.samplesPerBeat : 0.5 * sampleRate_;

    const auto division = static_cast<double> (params_.divisionBeats);
    const double divisionBeats = std::isfinite (division) && division > 0.0 ? division : 0.125;
    const double duty = std::isfinite (params_.duty) ? static_cast<double> (std::clamp (params_.duty, minDuty, maxDuty)) : 0.5;

    // Far longer than any step; keeps the cycle arithmetic well away from overflow.
    constexpr double maxCycleLength = 1.0e12;

    Pattern pattern;
    pattern.stepStart = step.stepStart;
    pattern.cycleLength = std::clamp (divisionBeats * samplesPerBeat, minCycleSeconds * sampleRate_, maxCycleLength);
    pattern.openLength = duty * pattern.cycleLength;
    pattern.rampLength = std::max (1.0, std::min ({ rampSeconds * sampleRate_,
                                                    pattern.openLength,
                                                    pattern.cycleLength - pattern.openLength }));
    return pattern;
}

float Gate::Pattern::gainAt (int64_t now) const noexcept
{
    const auto elapsed = static_cast<double> (std::max<int64_t> (now - stepStart, 0));
    const auto cycleNumber = std::floor (elapsed / cycleLength);
    const auto position = std::clamp (elapsed - cycleNumber * cycleLength, 0.0, cycleLength);
    const double attackFrom = cycleNumber == 0.0 ? static_cast<double> (firstAttackFrom) : 0.0;

    double gain = 0.0;

    if (position < rampLength)
        gain = attackFrom + (1.0 - attackFrom) * raisedCosine (position / rampLength);
    else if (position < openLength)
        gain = 1.0;
    else if (position < openLength + rampLength)
        gain = 1.0 - raisedCosine ((position - openLength) / rampLength);

    if (hasBlend && now >= blendStart)
    {
        const auto progress = static_cast<double> (now - blendStart) / rampLength;

        if (progress < 1.0)
            gain = static_cast<double> (blendFrom) + (gain - static_cast<double> (blendFrom)) * raisedCosine (progress);
    }

    return static_cast<float> (gain);
}

void Gate::startPendingPattern (int64_t firstIndex) noexcept
{
    const bool wasSounding = hasPattern_ && firstIndex == processedEnd_;

    auto pattern = makePattern (pendingStep_);
    pattern.stepStart = std::min (pattern.stepStart, firstIndex);

    // Pick up from the gain the gate had reached, so a retrigger never jumps. After a gap the
    // engine was playing another source and crossfades from it itself.
    if (wasSounding)
    {
        if (firstIndex == pattern.stepStart)
        {
            pattern.firstAttackFrom = lastGain_;
        }
        else
        {
            pattern.hasBlend = true;
            pattern.blendStart = firstIndex;
            pattern.blendFrom = lastGain_;
        }
    }

    pattern_ = pattern;
    hasPattern_ = true;
    hasPendingTrigger_ = false;
}

void Gate::process (const CaptureBuffer& /*capture*/, StereoBlock block, int64_t blockStartIndex) noexcept
{
    if (hasPendingTrigger_)
    {
        startPendingPattern (blockStartIndex);
    }
    else if (! hasPattern_)
    {
        // Processed without a trigger (e.g. straight after reset): free-run from here at 120 bpm.
        StepContext step;
        step.stepStart = blockStartIndex;
        step.samplesPerBeat = 0.5 * sampleRate_;
        pattern_ = makePattern (step);
        hasPattern_ = true;
    }

    for (int i = 0; i < block.numSamples; ++i)
    {
        const float gain = pattern_.gainAt (blockStartIndex + i);
        block.left[i] *= gain;
        block.right[i] *= gain;
        lastGain_ = gain;
    }

    processedEnd_ = blockStartIndex + block.numSamples;
}

} // namespace splinter
