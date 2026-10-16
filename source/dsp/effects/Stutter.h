#pragma once

#include "../EffectParams.h"
#include "../GlitchEffect.h"

#include <cstdint>
#include <limits>

namespace splinter
{

/** Loops the opening slice of the step until the step ends.

    The first pass plays the live input while the slice is being captured, then the
    slice repeats. Near the end of every pass the slice crossfades into the audio just
    before it, which runs straight on into the slice start after the wrap, so loop
    points never click. The playback rate glides to pitchGlideSemitones by the end of
    the step, and each repeat is quieter than the last according to decay.

    A rising glide would overtake the live input on the first pass, so the rate is held
    at 1 until the slice has been captured. The read position is a closed-form function
    of the absolute sample index, so the output does not depend on block size and a
    voice can start part-way through its step (after a transport jump). A retrigger while
    sounding crossfades from the old voice, which keeps its own read head. */
class Stutter final : public GlitchEffect
{
public:
    const char* name() const noexcept override { return "Stutter"; }

    void prepare (double sampleRate, int maxBlockSize) override;
    void reset() noexcept override;
    void trigger (const StepContext& step) noexcept override;
    void process (const CaptureBuffer& capture, StereoBlock block, int64_t blockStartIndex) noexcept override;

    /** Read at trigger(), so a change takes effect from the next step. */
    void setParams (const StutterParams& params) noexcept { params_ = params; }

    /** Shortest slice, so tiny repeat lengths at fast tempos still loop something audible. */
    static constexpr double minSliceSamples = 32.0;

    /** Loop-point crossfade (at most a quarter of the slice) and retrigger crossfade. */
    static constexpr double crossfadeSeconds = 0.003;

    /** Attenuation of the step's last repeat at decay = 1. */
    static constexpr double decayRangeDb = 60.0;

    static constexpr double maxGlideSemitones = 24.0;

private:
    struct Voice
    {
        int64_t stepStart = 0;
        int64_t fadeInStart = 0;       // first sample of its crossfade from the voice before it
        double stepLength = 1.0;       // samples
        double sliceLength = 1.0;      // samples, <= stepLength
        double loopCrossfade = 0.0;    // samples of slice phase blended at each loop point
        double octavesPerSample = 0.0; // playback rate is 2^(octavesPerSample * elapsed)
        double logGainPerRepeat = 0.0; // natural-log gain step per repeat, <= 0

        /** Integral of the glide rate from the step start to `elapsed`. */
        double glideIntegral (double elapsed) const noexcept;

        /** How far the read head has moved through the unwrapped slice after `elapsed` samples. */
        double readDistance (double elapsed) const noexcept;

        float repeatGain (double repeat) const noexcept;
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

    StutterParams params_;
    Voice voices_[maxVoices]; // newest first
    Voice pendingVoice_;
    int numVoices_ = 0;
    bool hasPendingVoice_ = false;
    int64_t processedEnd_ = std::numeric_limits<int64_t>::min(); // absolute index after the last processed sample
    int crossfadeLength_ = 1;
    double sampleRate_ = 44100.0;
};

} // namespace splinter
