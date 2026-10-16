#include "Crusher.h"

#include <algorithm>
#include <cmath>

namespace splinter
{

void Crusher::prepare (double /*sampleRate*/, int /*maxBlockSize*/)
{
    reset();
}

void Crusher::reset() noexcept
{
    hasPendingTrigger_ = false;
    anchor_ = 0;
    gridFactor_ = 1.0;
    processedEnd_ = std::numeric_limits<int64_t>::min();
}

void Crusher::trigger (const StepContext& step) noexcept
{
    pendingStep_ = step;
    hasPendingTrigger_ = true;
}

float Crusher::quantiserStep (float bits) noexcept
{
    const float clampedBits = std::isfinite (bits) ? std::clamp (bits, minBits, maxBits) : maxBits;
    return 2.0f / (std::exp2 (clampedBits) + 1.0f);
}

float Crusher::quantise (float input, float step) noexcept
{
    if (! std::isfinite (input))
        return 0.0f;

    // std::round is symmetric (halves round away from zero), so the quantiser is too.
    const float level = std::round (input / step) * step;
    const float limit = std::max (1.0f, std::fabs (input));
    return std::clamp (level, -limit, limit);
}

double Crusher::clampedDownsample() const noexcept
{
    const auto factor = static_cast<double> (params_.downsample);
    return std::isfinite (factor) ? std::clamp (factor, minDownsample, maxDownsample) : minDownsample;
}

void Crusher::startGrid (int64_t anchor, double factor) noexcept
{
    anchor_ = anchor;
    gridFactor_ = factor;
}

int64_t Crusher::heldIndex (int64_t now) const noexcept
{
    if (now <= anchor_)
        return now;

    const auto elapsed = static_cast<double> (now - anchor_);
    const auto holdNumber = std::floor (elapsed / gridFactor_);
    auto offset = std::ceil (holdNumber * gridFactor_);

    // The division can round up onto the next hold point when it is a hair in the future.
    if (offset > elapsed)
        offset = std::ceil ((holdNumber - 1.0) * gridFactor_);

    return anchor_ + static_cast<int64_t> (offset);
}

void Crusher::process (const CaptureBuffer& capture, StereoBlock block, int64_t blockStartIndex) noexcept
{
    const double factor = clampedDownsample();
    const bool continuing = blockStartIndex == processedEnd_;

    if (hasPendingTrigger_)
    {
        hasPendingTrigger_ = false;

        // A retrigger while running keeps the grid, so back-to-back Crusher steps do not tick.
        if (! continuing)
            startGrid (std::min (pendingStep_.stepStart, blockStartIndex), factor);
    }
    else if (! continuing)
    {
        startGrid (blockStartIndex, factor);
    }

    if (factor != gridFactor_)
        startGrid (heldIndex (blockStartIndex), factor);

    const float step = quantiserStep (params_.bits);

    for (int i = 0; i < block.numSamples; ++i)
    {
        const auto source = heldIndex (blockStartIndex + i);
        block.left[i] = quantise (capture.sample (0, source), step);
        block.right[i] = quantise (capture.sample (1, source), step);
    }

    processedEnd_ = blockStartIndex + block.numSamples;
}

} // namespace splinter
