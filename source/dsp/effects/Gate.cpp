#include "Gate.h"

namespace splinter
{

// STUB: passes the dry signal through. Replace with the real implementation.

void Gate::prepare (double sampleRate, int /*maxBlockSize*/)
{
    sampleRate_ = sampleRate;
}

void Gate::reset() noexcept {}

void Gate::trigger (const StepContext& step) noexcept
{
    step_ = step;
}

void Gate::process (const CaptureBuffer& /*capture*/, StereoBlock /*block*/, int64_t /*blockStartIndex*/) noexcept {}

} // namespace splinter
