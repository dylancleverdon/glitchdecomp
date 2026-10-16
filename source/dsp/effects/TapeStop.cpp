#include "TapeStop.h"

namespace splinter
{

// STUB: passes the dry signal through. Replace with the real implementation.

void TapeStop::prepare (double sampleRate, int /*maxBlockSize*/)
{
    sampleRate_ = sampleRate;
}

void TapeStop::reset() noexcept {}

void TapeStop::trigger (const StepContext& step) noexcept
{
    step_ = step;
}

void TapeStop::process (const CaptureBuffer& /*capture*/, StereoBlock /*block*/, int64_t /*blockStartIndex*/) noexcept {}

} // namespace splinter
