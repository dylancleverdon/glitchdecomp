#include "Stretch.h"

#include <algorithm>
#include <cmath>

namespace splinter
{

namespace
{
    constexpr double pi = 3.14159265358979323846;

    double clampOr (double value, double low, double high, double fallback) noexcept
    {
        return std::isnan (value) ? fallback : std::clamp (value, low, high);
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

    int64_t floorDivide (int64_t numerator, int64_t denominator) noexcept
    {
        const auto quotient = numerator / denominator;
        return (numerator % denominator != 0 && numerator < 0) ? quotient - 1 : quotient;
    }
} // namespace

void Stretch::prepare (double sampleRate, int /*maxBlockSize*/)
{
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
    crossfadeLength_ = std::max (1, static_cast<int> (std::lround (crossfadeSeconds * sampleRate_)));
    reset();
}

void Stretch::reset() noexcept
{
    voice_ = {};
    previousVoice_ = {};
    crossfadeStart_ = 0;
    processedEnd_ = 0;
}

void Stretch::trigger (const StepContext& step) noexcept
{
    // Only crossfade if this effect was rendering right up to the new step. Otherwise
    // another source was playing and the engine fades between them.
    previousVoice_ = voice_;
    previousVoice_.active = voice_.active && processedEnd_ >= step.stepStart;
    crossfadeStart_ = std::max (step.stepStart, processedEnd_);

    const auto stepLength = static_cast<double> (std::max<int64_t> (1, step.stepLength));
    const auto grainMs = clampOr (params_.grainMs, 10.0, 250.0, 80.0);

    // A grain longer than half the step would leave most of the step dry (the first
    // stretched grain only starts half a grain in), so cap it there.
    const auto grainSamples = std::min (grainMs * 0.001 * sampleRate_, 0.5 * stepLength);

    voice_.stepStart = step.stepStart;
    voice_.factor = clampOr (params_.factor, 1.0, 16.0, 4.0);
    voice_.hop = std::max<int64_t> (1, static_cast<int64_t> (0.5 * grainSamples));
    voice_.grainLength = 2 * voice_.hop;
    voice_.active = true;
}

int64_t Stretch::grainSourceStart (const Voice& voice, int64_t grainIndex) noexcept
{
    // Before the step the stretch runs at factor 1, so these grains read the live input.
    if (grainIndex < 0)
        return voice.stepStart + grainIndex * voice.hop;

    // Rounding down keeps the read at or behind now, and lets grains read whole samples.
    const auto centreOffset = static_cast<double> (grainIndex * voice.hop) / voice.factor;
    return voice.stepStart + static_cast<int64_t> (std::floor (centreOffset));
}

void Stretch::renderVoice (const Voice& voice, const CaptureBuffer& capture, int64_t now, float& left, float& right) const noexcept
{
    // At 50% overlap two grains sound at once: the newer one in the rising half of its
    // window, the older one in the falling half.
    const auto elapsed = now - voice.stepStart;
    const auto newerGrain = floorDivide (elapsed, voice.hop);
    const auto offset = elapsed - newerGrain * voice.hop;

    const auto newerRead = grainSourceStart (voice, newerGrain) + offset;
    const auto olderRead = grainSourceStart (voice, newerGrain - 1) + offset + voice.hop;

    // Periodic Hann: sin^2 (pi n / N) for the newer grain, and 1 minus that for the older one.
    const auto windowPhase = std::sin (pi * static_cast<double> (offset) / static_cast<double> (voice.grainLength));
    const auto newerWeight = static_cast<float> (windowPhase * windowPhase);

    const auto olderLeft = capture.sample (0, olderRead);
    const auto olderRight = capture.sample (1, olderRead);
    left = olderLeft + newerWeight * (capture.sample (0, newerRead) - olderLeft);
    right = olderRight + newerWeight * (capture.sample (1, newerRead) - olderRight);
}

void Stretch::process (const CaptureBuffer& capture, StereoBlock block, int64_t blockStartIndex) noexcept
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
