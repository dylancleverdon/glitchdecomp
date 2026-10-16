#include "Crusher.h"

namespace splinter
{

// STUB: passes the dry signal through. Replace with the real implementation.

void Crusher::prepare (double sampleRate, int /*maxBlockSize*/)
{
    sampleRate_ = sampleRate;
}

void Crusher::reset() noexcept {}

void Crusher::trigger (const StepContext& step) noexcept
{
    step_ = step;
}

void Crusher::process (const CaptureBuffer& /*capture*/, StereoBlock /*block*/, int64_t /*blockStartIndex*/) noexcept {}

} // namespace splinter
