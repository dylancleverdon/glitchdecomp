#include "Reverse.h"

#include <algorithm>
#include <cmath>

namespace splinter
{

namespace
{
    constexpr double pi = 3.14159265358979323846;

    // Keeps every position exactly representable in a double, whatever the engine sends.
    constexpr int64_t maxStepLength = int64_t { 1 } << 40;

    /** 0 -> 1 with zero slope at both ends; complementary crossfade weights sum to one. */
    double raisedCosine (double x) noexcept
    {
        return 0.5 - 0.5 * std::cos (pi * std::clamp (x, 0.0, 1.0));
    }

    /** Positions are at most stepStart - 1, and fractional ones at most stepStart - 1.5,
        so the cubic's two samples of look-ahead reach stepStart at the latest: never
        later than the live input. */
    float readPast (const CaptureBuffer& capture, int channel, double position) noexcept
    {
        const auto whole = std::floor (position);

        if (whole == position)
            return capture.sample (channel, static_cast<int64_t> (whole));

        return capture.sampleInterpolated (channel, position);
    }
} // namespace

void Reverse::prepare (double sampleRate, int /*maxBlockSize*/)
{
    sampleRate_ = sampleRate > 0.0 && std::isfinite (sampleRate) ? sampleRate : 44100.0;
    fadeLength_ = std::max (1, static_cast<int> (std::lround (fadeSeconds * sampleRate_)));
    reset();
}

void Reverse::reset() noexcept
{
    numVoices_ = 0;
    hasPendingVoice_ = false;
    processedEnd_ = std::numeric_limits<int64_t>::min();
}

void Reverse::trigger (const StepContext& step) noexcept
{
    const auto stepLength = std::clamp<int64_t> (step.stepLength, 1, maxStepLength);
    const auto speed = static_cast<double> (params_.speed);

    Voice voice;
    voice.stepStart = step.stepStart;
    voice.stepEnd = step.stepStart + stepLength;
    voice.fadeLength = static_cast<int> (std::clamp<int64_t> (stepLength / 2, 1, fadeLength_));
    voice.speed = std::isfinite (speed) ? std::clamp (speed, minSpeed, maxSpeed) : 1.0;

    pendingVoice_ = voice;
    hasPendingVoice_ = true;
}

void Reverse::startPendingVoice (int64_t firstIndex) noexcept
{
    // Only crossfade from the old voices if they were sounding right up to this sample; after
    // a gap the engine was playing another source and does that crossfade itself.
    if (firstIndex != processedEnd_)
        numVoices_ = 0;

    // With every slot busy the oldest voice, by now mostly faded out, is dropped.
    numVoices_ = std::min (numVoices_ + 1, maxVoices);

    for (int voice = numVoices_ - 1; voice > 0; --voice)
        voices_[voice] = voices_[voice - 1];

    voices_[0] = pendingVoice_;
    voices_[0].fadeInStart = std::max (firstIndex, pendingVoice_.stepStart);
    hasPendingVoice_ = false;
}

float Reverse::Voice::fadeInGain (int64_t now) const noexcept
{
    return static_cast<float> (raisedCosine (static_cast<double> (now - fadeInStart) / static_cast<double> (fadeLength)));
}

Reverse::Frame Reverse::renderVoice (const Voice& voice, const CaptureBuffer& capture, int64_t now) noexcept
{
    const double fadeOut = raisedCosine (static_cast<double> (voice.stepEnd - now) / static_cast<double> (voice.fadeLength));
    const auto gain = voice.fadeInGain (now) * static_cast<float> (fadeOut);

    // Silent before fadeInStart (which is never before stepStart), so reads stay in the past.
    if (gain <= 0.0f)
        return {};

    const double position = static_cast<double> (voice.stepStart - 1)
                          - static_cast<double> (now - voice.stepStart) * voice.speed;

    return { gain * readPast (capture, 0, position), gain * readPast (capture, 1, position) };
}

void Reverse::process (const CaptureBuffer& capture, StereoBlock block, int64_t blockStartIndex) noexcept
{
    if (hasPendingVoice_)
        startPendingVoice (blockStartIndex);

    if (numVoices_ > 0)
    {
        for (int i = 0; i < block.numSamples; ++i)
        {
            const int64_t now = blockStartIndex + i;
            Frame mix;
            float remaining = 1.0f; // weight left over for the older voices

            // A voice's own fade-in doubles as its crossfade over the voices before it.
            for (int voice = 0; voice < numVoices_; ++voice)
            {
                const auto frame = renderVoice (voices_[voice], capture, now);
                mix.left += remaining * frame.left;
                mix.right += remaining * frame.right;
                remaining -= remaining * voices_[voice].fadeInGain (now);

                if (remaining <= 0.0f)
                {
                    numVoices_ = voice + 1; // everything older has faded out
                    break;
                }
            }

            block.left[i] = mix.left;
            block.right[i] = mix.right;
        }
    }

    processedEnd_ = blockStartIndex + block.numSamples;
}

} // namespace splinter
