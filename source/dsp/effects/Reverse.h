#pragma once

#include "../EffectParams.h"
#include "../GlitchEffect.h"

namespace splinter
{

class Reverse final : public GlitchEffect
{
public:
    const char* name() const noexcept override { return "Reverse"; }

    void prepare (double sampleRate, int maxBlockSize) override;
    void reset() noexcept override;
    void trigger (const StepContext& step) noexcept override;
    void process (const CaptureBuffer& capture, StereoBlock block, int64_t blockStartIndex) noexcept override;

    void setParams (const ReverseParams& params) noexcept { params_ = params; }

private:
    ReverseParams params_;
    StepContext step_;
    double sampleRate_ = 44100.0;
};

} // namespace splinter
