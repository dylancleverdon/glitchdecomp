#include "TapeStop.h"

#include <algorithm>
#include <cmath>

namespace splinter
{

namespace
{
    constexpr double pi = 3.14159265358979323846;

    // Decay rate of the exponential speed curve at curve = 1. A rate of 5 brings the speed
    // down to about a third in the first fifth of the stop: a quick dive, then a long low tail.
    constexpr double maxCurveRate = 5.0;

    // Below this the exponential formulas lose precision, and they equal the linear ramp anyway.
    constexpr double minCurveRate = 1.0e-6;

    double clampOr (double value, double low, double high, double fallback) noexcept
    {
        return std::isnan (value) ? fallback : std::clamp (value, low, high);
    }

    double curveRate (double curve) noexcept
    {
        return maxCurveRate * clampOr (curve, 0.0, 1.0, 0.0);
    }

    /** Raised-cosine weight of the new signal at sample `position` of a crossfade. */
    float crossfadeWeight (int64_t position, int length) noexcept
    {
        if (position < 0)
            return 0.0f;

        if (position >= length)
            return 1.0f;

        const auto x = (static_cast<double> (position) + 1.0) / (static_cast<double> (length) + 1.0);
        return static_cast<float> (0.5 - 0.5 * std::cos (pi * x));
    }

    float readAt (const CaptureBuffer& capture, int channel, double position) noexcept
    {
        const auto whole = std::floor (position);

        if (whole == position)
            return capture.sample (channel, static_cast<int64_t> (whole));

        return capture.sampleInterpolated (channel, position);
    }
} // namespace

double TapeStop::speedAt (double progress, double curve) noexcept
{
    if (! (progress > 0.0))
        return 1.0;

    if (progress >= 1.0)
        return 0.0;

    const auto rate = curveRate (curve);

    if (rate < minCurveRate)
        return 1.0 - progress;

    // exp (-rate * progress), shifted and scaled so it reaches exactly 0 at the end
    return (std::exp (-rate * progress) - std::exp (-rate)) / -std::expm1 (-rate);
}

double TapeStop::distanceAt (double progress, double curve) noexcept
{
    if (progress <= 0.0)
        return progress;

    progress = std::min (progress, 1.0);
    const auto rate = curveRate (curve);

    if (rate < minCurveRate)
        return progress - 0.5 * progress * progress;

    return (-std::expm1 (-rate * progress) / rate - progress * std::exp (-rate)) / -std::expm1 (-rate);
}

void TapeStop::prepare (double sampleRate, int /*maxBlockSize*/)
{
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
    crossfadeLength_ = std::max (1, static_cast<int> (std::lround (crossfadeSeconds * sampleRate_)));
    reset();
}

void TapeStop::reset() noexcept
{
    voice_ = {};
    previousVoice_ = {};
    crossfadeStart_ = 0;
    processedEnd_ = 0;
}

void TapeStop::trigger (const StepContext& step) noexcept
{
    // Only crossfade if this effect was rendering right up to the new step. Otherwise
    // another source was playing and the engine fades between them.
    previousVoice_ = voice_;
    previousVoice_.active = voice_.active && processedEnd_ >= step.stepStart;
    crossfadeStart_ = std::max (step.stepStart, processedEnd_);

    const auto stepLength = static_cast<double> (std::max<int64_t> (1, step.stepLength));
    const auto duration = clampOr (params_.duration, 0.05, 1.0, 1.0);

    voice_.stepStart = step.stepStart;
    voice_.stopLength = std::max (1.0, duration * stepLength);
    voice_.fadeLength = std::min (voice_.stopLength, std::max (fadeFraction * voice_.stopLength, fadeMinSeconds * sampleRate_));
    voice_.curve = clampOr (params_.curve, 0.0, 1.0, 0.5);
    voice_.active = true;
}

void TapeStop::renderVoice (const Voice& voice, const CaptureBuffer& capture, int64_t now, float& left, float& right) const noexcept
{
    const auto elapsed = static_cast<double> (now - voice.stepStart);

    if (elapsed >= voice.stopLength)
    {
        left = 0.0f;
        right = 0.0f;
        return;
    }

    // Speed never exceeds 1, so the head never travels further than the elapsed time,
    // which keeps it at or behind now - 1.
    const auto travelled = std::min (voice.stopLength * distanceAt (elapsed / voice.stopLength, voice.curve), elapsed);
    const auto position = static_cast<double> (voice.stepStart - 1) + travelled;

    // Near the end the head barely moves and would hold a DC value, so fade it out.
    const auto fadeStart = voice.stopLength - voice.fadeLength;
    auto gain = 1.0f;

    if (elapsed > fadeStart)
        gain = static_cast<float> (0.5 + 0.5 * std::cos (pi * (elapsed - fadeStart) / voice.fadeLength));

    left = gain * readAt (capture, 0, position);
    right = gain * readAt (capture, 1, position);
}

void TapeStop::process (const CaptureBuffer& capture, StereoBlock block, int64_t blockStartIndex) noexcept
{
    // Never triggered since reset: nothing to play, leave the dry input in place.
    if (! voice_.active)
        return;

    for (int i = 0; i < block.numSamples; ++i)
    {
        const auto now = blockStartIndex + i;
        float left = 0.0f, right = 0.0f;
        renderVoice (voice_, capture, now, left, right);

        if (previousVoice_.active && now < crossfadeStart_ + crossfadeLength_)
        {
            float oldLeft = 0.0f, oldRight = 0.0f;
            renderVoice (previousVoice_, capture, now, oldLeft, oldRight);

            const auto weight = crossfadeWeight (now - crossfadeStart_, crossfadeLength_);
            left = oldLeft + weight * (left - oldLeft);
            right = oldRight + weight * (right - oldRight);
        }

        block.left[i] = left;
        block.right[i] = right;
    }

    processedEnd_ = blockStartIndex + block.numSamples;
}

} // namespace splinter
