#include "GlitchEngine.h"

namespace splinter
{

// STUB: passes audio through untouched. Replace with the real engine.

GlitchEngine::GlitchEngine() = default;
GlitchEngine::~GlitchEngine() = default;

void GlitchEngine::prepare (double /*sampleRate*/, int /*maxBlockSize*/) {}

void GlitchEngine::reset() noexcept
{
    currentStep_.store (-1);
    currentEffect_.store (0);
}

void GlitchEngine::setParams (const EngineParams& params) noexcept
{
    params_ = params;
}

void GlitchEngine::process (StereoBlock /*block*/, const TransportInfo& /*transport*/) noexcept {}

} // namespace splinter
