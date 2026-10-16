#pragma once

#include "dsp/EffectParams.h"

#include <cstdint>
#include <vector>

namespace splinter::render
{

/** What the render tool tells the engine for the block starting at `blockStartSample`: the
    transport is playing and its PPQ position counts from 0 at the first sample. */
TransportInfo transportForBlock (int64_t blockStartSample, double sampleRate, double bpm) noexcept;

/** Runs a stereo signal through a freshly prepared GlitchEngine, in place, in blocks of
    blockSize samples (the last one may be shorter), as a host playing from bar 1 would. */
void renderThroughEngine (const EngineParams& params, double sampleRate, double bpm, int blockSize,
                          std::vector<float>& left, std::vector<float>& right);

} // namespace splinter::render
