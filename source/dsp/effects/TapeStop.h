#pragma once

#include "../EffectParams.h"
#include "../GlitchEffect.h"

namespace splinter
{

/** A read head that starts at the live input and slows to a halt over
    duration * stepLength samples, dropping the pitch with it. The level fades to
    zero as the head stops, then the output is silent until the next trigger.

    The head trails the live input by one sample, so the 4-point interpolation
    never touches samples later than `now`. Everything is a closed-form function of
    the absolute sample index, which makes the output independent of block size. */
class TapeStop final : public GlitchEffect
{
public:
    const char* name() const noexcept override { return "Tape Stop"; }

    void prepare (double sampleRate, int maxBlockSize) override;
    void reset() noexcept override;
    void trigger (const StepContext& step) noexcept override;
    void process (const CaptureBuffer& capture, StereoBlock block, int64_t blockStartIndex) noexcept override;

    /** Read at trigger(), so a change takes effect from the next step. */
    void setParams (const TapeStopParams& params) noexcept { params_ = params; }

    /** Playback speed at `progress` (0..1) through the stop: 1 at the start, 0 at the end.
        curve 0 is a linear ramp; higher values bend it towards an exponential decay. */
    static double speedAt (double progress, double curve) noexcept;

    /** Distance the head has travelled by `progress`, in stop lengths: the integral of speedAt().
        Before the stop (progress < 0) the head runs at full speed. */
    static double distanceAt (double progress, double curve) noexcept;

    /** Part of the stop, at its end, over which the level fades to silence (at least fadeMinSeconds). */
    static constexpr double fadeFraction = 0.25;
    static constexpr double fadeMinSeconds = 0.003;

    /** Crossfade from the previous stop when retriggered while sounding. */
    static constexpr double crossfadeSeconds = 0.003;

private:
    struct Voice
    {
        int64_t stepStart = 0;
        double stopLength = 1.0; // samples
        double fadeLength = 1.0; // samples, <= stopLength
        double curve = 0.0;
        bool active = false;
    };

    void renderVoice (const Voice& voice, const CaptureBuffer& capture, int64_t now, float& left, float& right) const noexcept;

    TapeStopParams params_;
    Voice voice_, previousVoice_;
    int64_t crossfadeStart_ = 0;
    int64_t processedEnd_ = 0; // absolute index after the last processed sample
    int crossfadeLength_ = 1;
    double sampleRate_ = 44100.0;
};

} // namespace splinter
