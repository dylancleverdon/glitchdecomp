#pragma once

#include "../EffectParams.h"
#include "../GlitchEffect.h"

namespace splinter
{

/** Pitch-preserving granular time stretch.

    The read centre moves at 1/factor from the step start: sourcePos(e) = stepStart + e / factor.
    A grain starts every hop = grainLength / 2 samples, at the read centre of its start
    time (rounded down to a whole sample), and reads forward at normal speed under a
    periodic Hann window. At 50% overlap those windows sum to exactly 1.

    A grain that starts at elapsed e reads stepStart + floor (e / factor) + k while
    now = stepStart + e + k, so with factor >= 1 it never reads ahead of now. Grains
    that would start before the step read the live input, as if the stretch ran at
    factor 1 until the step began: the first half grain is exactly the dry input, and
    factor 1 is an exact identity. */
class Stretch final : public GlitchEffect
{
public:
    const char* name() const noexcept override { return "Stretch"; }

    void prepare (double sampleRate, int maxBlockSize) override;
    void reset() noexcept override;
    void trigger (const StepContext& step) noexcept override;
    void process (const CaptureBuffer& capture, StereoBlock block, int64_t blockStartIndex) noexcept override;

    /** Read at trigger(), so a change takes effect from the next step. */
    void setParams (const StretchParams& params) noexcept { params_ = params; }

    /** Crossfade from the previous stretch when retriggered while sounding. */
    static constexpr double crossfadeSeconds = 0.003;

private:
    struct Voice
    {
        int64_t stepStart = 0;
        double factor = 1.0;
        int64_t grainLength = 2; // samples, even
        int64_t hop = 1;         // grainLength / 2
        bool active = false;
    };

    static int64_t grainSourceStart (const Voice& voice, int64_t grainIndex) noexcept;
    void renderVoice (const Voice& voice, const CaptureBuffer& capture, int64_t now, float& left, float& right) const noexcept;

    StretchParams params_;
    Voice voice_, previousVoice_;
    int64_t crossfadeStart_ = 0;
    int64_t processedEnd_ = 0; // absolute index after the last processed sample
    int crossfadeLength_ = 1;
    double sampleRate_ = 44100.0;
};

} // namespace splinter
