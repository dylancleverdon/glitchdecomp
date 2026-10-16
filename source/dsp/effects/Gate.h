#pragma once

#include "../EffectParams.h"
#include "../GlitchEffect.h"

#include <cstdint>
#include <limits>

namespace splinter
{

/** Rhythmic gate. Cycles of divisionBeats start at the step start; each cycle opens with a
    raised-cosine attack at its very beginning, stays open until duty * cycle, then closes with
    an equally long release. Both ramps lie inside the cycle, the half-gain points are exactly
    duty * cycle apart, and the mean gain over a cycle is duty.

    The gain is a closed-form function of the absolute sample index, so the output does not
    depend on block size. A retrigger while the gate is sounding never jumps: on time, the new
    step's first attack starts from the gain the gate had reached; a late retrigger (after a
    transport jump) blends from that gain into the new pattern over one ramp length.

    Parameters are read at trigger(), so a change takes effect from the next step. */
class Gate final : public GlitchEffect
{
public:
    const char* name() const noexcept override { return "Gate"; }

    void prepare (double sampleRate, int maxBlockSize) override;
    void reset() noexcept override;
    void trigger (const StepContext& step) noexcept override;
    void process (const CaptureBuffer& capture, StereoBlock block, int64_t blockStartIndex) noexcept override;

    void setParams (const GateParams& params) noexcept { params_ = params; }

    static constexpr double rampSeconds = 0.001;
    static constexpr double minCycleSeconds = 0.002;
    static constexpr float minDuty = 0.05f;
    static constexpr float maxDuty = 0.95f;

private:
    struct Pattern
    {
        int64_t stepStart = 0;
        double cycleLength = 1.0;     // samples
        double openLength = 0.5;      // samples from the cycle start to the start of the release
        double rampLength = 1.0;      // samples, for both attack and release
        float firstAttackFrom = 0.0f; // gain the first attack of the step starts from
        bool hasBlend = false;
        int64_t blendStart = 0;
        float blendFrom = 0.0f;

        float gainAt (int64_t now) const noexcept;
    };

    Pattern makePattern (const StepContext& step) const noexcept;
    void startPendingPattern (int64_t firstIndex) noexcept;

    GateParams params_;
    StepContext pendingStep_;
    bool hasPendingTrigger_ = false;
    bool hasPattern_ = false;
    Pattern pattern_;
    float lastGain_ = 0.0f; // gain at processedEnd_ - 1
    int64_t processedEnd_ = std::numeric_limits<int64_t>::min();
    double sampleRate_ = 44100.0;
};

} // namespace splinter
