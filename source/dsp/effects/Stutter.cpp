#include "Stutter.h"

namespace splinter
{

// STUB: passes the dry signal through. Replace with the real implementation.

void Stutter::prepare (double sampleRate, int /*maxBlockSize*/)
{
    sampleRate_ = sampleRate;
}

void Stutter::reset() noexcept {}

void Stutter::trigger (const StepContext& step) noexcept
{
    step_ = step;
}

void Stutter::process (const CaptureBuffer& /*capture*/, StereoBlock /*block*/, int64_t /*blockStartIndex*/) noexcept {}

} // namespace splinter
