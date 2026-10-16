#include "Stretch.h"

namespace splinter
{

// STUB: passes the dry signal through. Replace with the real implementation.

void Stretch::prepare (double sampleRate, int /*maxBlockSize*/)
{
    sampleRate_ = sampleRate;
}

void Stretch::reset() noexcept {}

void Stretch::trigger (const StepContext& step) noexcept
{
    step_ = step;
}

void Stretch::process (const CaptureBuffer& /*capture*/, StereoBlock /*block*/, int64_t /*blockStartIndex*/) noexcept {}

} // namespace splinter
