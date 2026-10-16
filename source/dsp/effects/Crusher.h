#pragma once

#include "../EffectParams.h"
#include "../GlitchEffect.h"

#include <cstdint>
#include <limits>

namespace splinter
{

/** Bit-depth reduction plus a fractional sample-and-hold.

    The hold grid is anchored at an absolute sample index: hold point k is the input at
    anchor + ceil (k * downsample), and that sample is held until the next hold point. The
    output is therefore a function of absolute indices only, so it does not depend on how
    the host splits its blocks, and every read is at or before the sample being rendered.

    A trigger after a gap (the engine was playing something else) restarts the grid at the
    step start. A retrigger while the crusher is still running keeps the grid going, so
    consecutive Crusher steps sound like one continuous effect. Parameters apply live: a
    change of downsample re-anchors the grid on the sample currently held, so the new rate
    is measured from the last hold point instead of jumping to an unrelated phase. */
class Crusher final : public GlitchEffect
{
public:
    const char* name() const noexcept override { return "Crusher"; }

    void prepare (double sampleRate, int maxBlockSize) override;
    void reset() noexcept override;
    void trigger (const StepContext& step) noexcept override;
    void process (const CaptureBuffer& capture, StereoBlock block, int64_t blockStartIndex) noexcept override;

    /** Applies from the next process() call. */
    void setParams (const CrusherParams& params) noexcept { params_ = params; }

    static constexpr float minBits = 1.0f;
    static constexpr float maxBits = 16.0f;
    static constexpr double minDownsample = 1.0;
    static constexpr double maxDownsample = 64.0;

    /** Quantiser step for a (continuous) bit depth: 2 / (2^bits + 1). One bit still keeps the
        three levels 0 and +-2/3, so quiet material is not wiped out completely. */
    static float quantiserStep (float bits) noexcept;

    /** Mid-tread rounding to a multiple of step, so silence stays silent. The result is
        never louder than max (1, |input|): in-range input stays in [-1, 1]. */
    static float quantise (float input, float step) noexcept;

    /** Absolute index of the input sample held at `now`; always in [now - 2 * maxDownsample, now]. */
    int64_t heldIndex (int64_t now) const noexcept;

private:
    void startGrid (int64_t anchor, double factor) noexcept;
    double clampedDownsample() const noexcept;

    CrusherParams params_;
    StepContext pendingStep_;
    bool hasPendingTrigger_ = false;
    int64_t anchor_ = 0;      // absolute index of hold point 0
    double gridFactor_ = 1.0; // downsample factor the grid currently runs at
    int64_t processedEnd_ = std::numeric_limits<int64_t>::min(); // absolute index after the last processed sample
};

} // namespace splinter
