#pragma once

#include "../EffectParams.h"
#include "../GlitchEffect.h"

#include <cstdint>
#include <limits>

namespace splinter
{

/** Plays the audio just before the step start backwards: output at step offset t is
    the input at stepStart - 1 - t * speed. It reads nothing later than the step's first
    sample (the interpolator's look-ahead), so it is causal by construction.

    The level fades in from silence when a voice starts and fades out over the last few
    milliseconds of the step. A retrigger while sounding crossfades from the old voice
    (which keeps its own read head) to the new one. Positions are closed-form functions
    of the absolute sample index, so the output does not depend on block size. */
class Reverse final : public GlitchEffect
{
public:
    const char* name() const noexcept override { return "Reverse"; }

    void prepare (double sampleRate, int maxBlockSize) override;
    void reset() noexcept override;
    void trigger (const StepContext& step) noexcept override;
    void process (const CaptureBuffer& capture, StereoBlock block, int64_t blockStartIndex) noexcept override;

    /** Read at trigger(), so a change takes effect from the next step. */
    void setParams (const ReverseParams& params) noexcept { params_ = params; }

    /** Fade at the step edges and crossfade at retriggers (at most half the step). */
    static constexpr double fadeSeconds = 0.003;

    static constexpr double minSpeed = 0.5;
    static constexpr double maxSpeed = 2.0;

private:
    struct Voice
    {
        int64_t stepStart = 0;
        int64_t stepEnd = 1;     // first sample after the step
        int64_t fadeInStart = 0; // first sample the voice rendered (never before stepStart)
        int fadeLength = 1;      // samples
        double speed = 1.0;

        float fadeInGain (int64_t now) const noexcept;
    };

    struct Frame
    {
        float left = 0.0f;
        float right = 0.0f;
    };

    /** The sounding voice plus up to two older ones still fading out, so even a retrigger
        that lands inside a crossfade stays smooth. */
    static constexpr int maxVoices = 3;

    void startPendingVoice (int64_t firstIndex) noexcept;
    static Frame renderVoice (const Voice& voice, const CaptureBuffer& capture, int64_t now) noexcept;

    ReverseParams params_;
    Voice voices_[maxVoices]; // newest first
    Voice pendingVoice_;
    int numVoices_ = 0;
    bool hasPendingVoice_ = false;
    int64_t processedEnd_ = std::numeric_limits<int64_t>::min(); // absolute index after the last processed sample
    int fadeLength_ = 1;
    double sampleRate_ = 44100.0;
};

} // namespace splinter
