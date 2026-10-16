#include "Reverse.h"

namespace splinter
{

// STUB: passes the dry signal through. Replace with the real implementation.

void Reverse::prepare (double sampleRate, int /*maxBlockSize*/)
{
    sampleRate_ = sampleRate;
}

void Reverse::reset() noexcept {}

void Reverse::trigger (const StepContext& step) noexcept
{
    step_ = step;
}

void Reverse::process (const CaptureBuffer& /*capture*/, StereoBlock /*block*/, int64_t /*blockStartIndex*/) noexcept {}

} // namespace splinter
