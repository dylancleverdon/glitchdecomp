#pragma once

#include "AudioBlock.h"
#include "CaptureBuffer.h"
#include "EffectParams.h"
#include "GlitchEffect.h"
#include "StepSequencer.h"
#include "effects/Crusher.h"
#include "effects/Gate.h"
#include "effects/Reverse.h"
#include "effects/Stretch.h"
#include "effects/Stutter.h"
#include "effects/TapeStop.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

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
    /** Ramps linearly to a new target over a fixed number of samples, then holds it exactly. */
    class LinearSmoother
    {
    public:
        void setRampLength (int samples) noexcept { rampLength_ = samples > 0 ? samples : 1; }
        void setTarget (float target) noexcept;
        void snapTo (float value) noexcept;
        float next() noexcept;

    private:
        float current_ = 0.0f, target_ = 0.0f, increment_ = 0.0f;
        int remaining_ = 0, rampLength_ = 1;
    };

    /** One source (an effect, or None for dry) in the stack of crossfades. Each layer
        fades in over everything below it; once a layer is fully in, the ones below it
        are inaudible and dropped. Nested fades keep the weights summing to one even
        when a step change lands in the middle of another crossfade. */
    struct SourceLayer
    {
        EffectType source = EffectType::None;
        int fadeElapsed = 0; // samples of this layer's fade-in rendered so far (ignored for the bottom layer)
    };

    static constexpr int maxLayers = 4;
    static constexpr double crossfadeSeconds = 0.003;
    static constexpr double smoothingSeconds = 0.02;
    static constexpr double captureSeconds = 12.0;
    static constexpr int maxChunkSize = 65536;

    void applyParams() noexcept;
    void processChunk (StereoBlock block, const TransportInfo& transport) noexcept;
    void startStep (const StepSegment& segment) noexcept;
    void pushLayer (EffectType source) noexcept;
    void renderSegment (StereoBlock block, int64_t startIndex) noexcept;
    void renderSpan (StereoBlock block, int64_t startIndex) noexcept;
    void retireFadedLayers() noexcept;
    GlitchEffect* effectFor (EffectType type) const noexcept;
    StereoBlock scratchFor (EffectType type, int numSamples) noexcept;

    EngineParams params_;

    double sampleRate_ = 44100.0;
    int maxBlockSize_ = 0; // 0 until prepare(); process() leaves the audio alone before that

    CaptureBuffer capture_;
    StepSequencer sequencer_;

    Stutter stutter_;
    Reverse reverse_;
    TapeStop tapeStop_;
    Stretch stretch_;
    Crusher crusher_;
    Gate gate_;
    std::array<GlitchEffect*, numEffectTypes> effects_ {}; // indexed by EffectType, None stays null

    std::vector<float> scratch_; // a stereo wet buffer of maxBlockSize_ per effect type
    std::vector<float> fadeCurve_; // fade-in gain for each sample of a crossfade
    int fadeLength_ = 1;

    std::array<SourceLayer, maxLayers> layers_ {};
    int numLayers_ = 1;

    LinearSmoother mixSmoother_, gainSmoother_;
    bool snapSmoothers_ = true;

    friend struct GlitchEngineTestAccess; // lets the unit tests swap in instrumented effects

    std::atomic<int> currentStep_ { -1 };
    std::atomic<int> currentEffect_ { 0 };
};

} // namespace splinter
