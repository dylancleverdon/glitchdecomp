#pragma once

#include "AudioBlock.h"
#include "CaptureBuffer.h"
#include "EffectParams.h"

#include <atomic>

namespace splinter
{

/** The whole glitch processor: capture buffer, step sequencer, the effects and
    the dry/wet mixing between them. Framework independent; the plugin and the
    offline render tool both drive it.

    Threading: prepare() and reset() run on the message thread while audio is
    stopped. setParams() and process() run on the audio thread. currentStep() and
    currentEffect() may be read from any thread. */
class GlitchEngine
{
public:
    GlitchEngine();
    ~GlitchEngine();

    GlitchEngine (const GlitchEngine&) = delete;
    GlitchEngine& operator= (const GlitchEngine&) = delete;

    void prepare (double sampleRate, int maxBlockSize);
    void reset() noexcept;

    /** Takes effect from the next process() call. */
    void setParams (const EngineParams& params) noexcept;

    /** Processes a block in place. block.numSamples may be anything from 0 to the
        maxBlockSize passed to prepare(). */
    void process (StereoBlock block, const TransportInfo& transport) noexcept;

    /** Step being played at the end of the last block, or -1 before the first block. */
    int currentStep() const noexcept { return currentStep_.load (std::memory_order_relaxed); }

    /** Effect sounding at the end of the last block (None if the step is empty or did not fire). */
    EffectType currentEffect() const noexcept { return static_cast<EffectType> (currentEffect_.load (std::memory_order_relaxed)); }

private:
    EngineParams params_;
    std::atomic<int> currentStep_ { -1 };
    std::atomic<int> currentEffect_ { 0 };
};

} // namespace splinter
