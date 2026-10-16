#pragma once

#include "AudioBlock.h"
#include "CaptureBuffer.h"

#include <cstdint>

namespace splinter
{

/** Timing of the sequencer step an effect was triggered for. */
struct StepContext
{
    int64_t stepStart = 0;           // absolute sample index of the step's first sample (may lie in an earlier block)
    int64_t stepLength = 1;          // step length in samples, always >= 1
    double sampleRate = 44100.0;
    double samplesPerBeat = 22050.0; // one quarter note
};

/** Base class for every glitch effect.

    Contract (see docs/ARCHITECTURE.md):
      - prepare() may allocate. Everything else must be real-time safe:
        no allocation, locks, I/O or exceptions.
      - trigger() is called once when a step that uses this effect begins. If the
        effect was already sounding (the same effect on consecutive steps), it must
        stay click-free across the retrigger by crossfading internally.
      - process() receives a block that already holds the dry input for the absolute
        indices [blockStartIndex, blockStartIndex + block.numSamples) and overwrites
        it with the fully wet signal. While rendering absolute index `now` it may
        read the capture buffer anywhere in [capture.oldestIndex(), now]; never
        later samples. The engine handles dry/wet mixing and the fades between
        different effects.
      - Output must stay finite and roughly within the input's level (no runaway gain). */
class GlitchEffect
{
public:
    virtual ~GlitchEffect() = default;

    virtual const char* name() const noexcept = 0;
    virtual void prepare (double sampleRate, int maxBlockSize) = 0;
    virtual void reset() noexcept = 0;
    virtual void trigger (const StepContext& step) noexcept = 0;
    virtual void process (const CaptureBuffer& capture, StereoBlock block, int64_t blockStartIndex) noexcept = 0;
};

} // namespace splinter
